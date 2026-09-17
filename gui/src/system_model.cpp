#include "system_model.hpp"

#include <QDateTime>
#include <QMetaObject>

namespace spear::gui {

// ---- EventListModel ------------------------------------------------------------

QVariant EventListModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= rowCount()) return {};
    const auto& r = rows_[static_cast<std::size_t>(idx.row())];
    switch (role) {
    case TimeRole:     return r.time;
    case KindRole:     return r.kind;
    case SourceRole:   return r.source;
    case DetailRole:   return r.detail;
    case SeverityRole: return r.severity;
    case RangeRole:    return r.range;
    default:           return {};
    }
}

QHash<int, QByteArray> EventListModel::roleNames() const {
    return {{TimeRole, "time"}, {KindRole, "kind"}, {SourceRole, "source"}, {DetailRole, "detail"},
            {SeverityRole, "severity"}, {RangeRole, "range"}};
}

namespace {
// 0 = info, 1 = notice(状態遷移等), 2 = warning, 3 = error/fault
int severity_of(const Event& e) {
    switch (e.kind) {
    case EventKind::Error: case EventKind::Overflow: case EventKind::OutOfSequence: case EventKind::Disconnected:
    case EventKind::ConsumerOverflow: case EventKind::Underflow: case EventKind::DiskLow: case EventKind::LoUnlock:
        return e.kind == EventKind::LoUnlock && e.value == 1 ? 1 : 3;
    case EventKind::Warning: case EventKind::Timeout: case EventKind::Discontinuity: case EventKind::ThermalThrottle:
    case EventKind::ClockReset:
        return 2;
    case EventKind::UhdLog:
        return e.value >= 4 ? 3 : (e.value == 3 ? 2 : 0); // uhd severity: warning=3, error=4
    case EventKind::DeviceState: case EventKind::Reconnected: case EventKind::StartupStage: case EventKind::Retune:
    case EventKind::TimeReference:
        return 1;
    default:
        return 0;
    }
}
} // namespace

void EventListModel::push(const Event& e) {
    Row r;
    r.time = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    r.kind = QString::fromUtf8(std::string(to_string(e.kind)).c_str()).toUpper();
    r.source = QString::fromStdString(e.source);
    r.detail = QString::fromStdString(e.detail);
    r.severity = severity_of(e);
    if (e.range.begin || e.range.end)
        r.range = QString("g%1 %2..%3").arg(e.range.generation).arg(e.range.begin).arg(e.range.end);
    beginInsertRows({}, 0, 0);
    rows_.push_front(std::move(r));
    endInsertRows();
    if (rows_.size() > kMax) {
        beginRemoveRows({}, kMax, kMax);
        rows_.pop_back();
        endRemoveRows();
    }
}

// ---- SystemModel --------------------------------------------------------------

SystemModel::SystemModel(Core& core, QObject* parent) : QObject(parent), core_(core) {
    // 既存の履歴を取り込む(古い順に push すると新しい順に並ぶ)
    auto hist = core_.events().history();
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) events_.push(*it);
    listener_ = core_.events().subscribe([this](const Event& e) {
        // dispatch thread → GUI thread
        QMetaObject::invokeMethod(this, [this, e] {
            events_.push(e);
            if (e.kind == EventKind::Retune) applied_freq_ = static_cast<double>(e.value);
            // 状態に関わる event は 250 ms のポーリングを待たず即時に Core を読み直す
            if (e.kind == EventKind::Retune || e.kind == EventKind::DeviceState || e.kind == EventKind::Reconnected ||
                e.kind == EventKind::StartupStage || (e.kind == EventKind::Info && e.source == "core"))
                timerEvent(nullptr);
            Q_EMIT eventArrived(severity_of(e), QString::fromUtf8(std::string(to_string(e.kind)).c_str()), QString::fromStdString(e.detail));
        }, Qt::QueuedConnection);
    });
    timer_ = startTimer(250);
    timerEvent(nullptr);
}

SystemModel::~SystemModel() {
    if (listener_) core_.events().unsubscribe(listener_);
}

void SystemModel::timerEvent(QTimerEvent*) {
    if (core_.has_source()) {
        ds_ = core_.source().device_status();
        cfg_ = core_.source().config();
        warming_ = core_.source().warming_up();
        report_.clear();
        for (const auto& l : core_.source().startup_report()) report_ << QString::fromStdString(l);
        state_version_ = core_.source().state_version();
        consumers_.clear();
        for (const auto& s : core_.rx().stats()) {
            QVariantMap m;
            m["name"] = QString::fromStdString(s.name);
            m["policy"] = s.policy == DeliveryPolicy::Lossless ? "LOSSLESS" : "LATEST";
            m["delivered"] = static_cast<qulonglong>(s.delivered_blocks);
            m["dropped"] = static_cast<qulonglong>(s.dropped_blocks);
            m["bursts"] = static_cast<qulonglong>(s.overflow_bursts);
            m["depth"] = static_cast<int>(s.queue_depth);
            m["capacity"] = static_cast<int>(s.queue_capacity);
            m["maxDepth"] = static_cast<int>(s.max_depth);
            consumers_.push_back(m);
        }
    }
    h_ = core_.health().latest();
    Q_EMIT changed();
}

QString SystemModel::clock() const { return QDateTime::currentDateTimeUtc().toString("HH:mm:ss'Z'"); }

void SystemModel::retune(double hz) {
    if (!core_.has_source()) return;
    core_.source().retune(hz);
}

QString SystemModel::formatFreq(double hz) const {
    if (hz >= 1e9) return QString::number(hz / 1e9, 'f', 6) + " GHz";
    if (hz >= 1e6) return QString::number(hz / 1e6, 'f', 6) + " MHz";
    return QString::number(hz / 1e3, 'f', 3) + " kHz";
}

} // namespace spear::gui
