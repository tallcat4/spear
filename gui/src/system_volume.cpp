#include "system_volume.hpp"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QRegularExpression>

namespace spear::gui {

namespace {
const QString kSink = QStringLiteral("@DEFAULT_AUDIO_SINK@");
const QStringList kGet{QStringLiteral("get-volume"), kSink};
} // namespace

SystemVolume::SystemVolume(QObject* parent) : QObject(parent) {
    proc_.setProgram(QStringLiteral("wpctl"));
    proc_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&proc_, &QProcess::finished, this, &SystemVolume::finished);
    connect(&proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        available_ = false;
        error_ = QStringLiteral("wpctl not found");
        queue_.clear();
        Q_EMIT changed();
    });
    if (QCoreApplication::instance()) QCoreApplication::instance()->installEventFilter(this);
    refresh();
}

void SystemVolume::step(int direction) {
    if (direction == 0) return;
    enqueue({QStringLiteral("set-mute"), kSink, QStringLiteral("0")});
    const QString amount = QString::number(static_cast<int>(kStep * 100)) + (direction > 0 ? "%+" : "%-");
    enqueue({QStringLiteral("set-volume"), QStringLiteral("-l"), QStringLiteral("1.0"), kSink, amount});
    enqueue(kGet);
}

void SystemVolume::toggleMute() {
    enqueue({QStringLiteral("set-mute"), kSink, QStringLiteral("toggle")});
    enqueue(kGet);
}

void SystemVolume::refresh() { enqueue(kGet); }

bool SystemVolume::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::KeyPress) {
        // 同じキー入力がウィンドウと各 Item に配られるので、ウィンドウに届いたものだけ扱う
        const auto* k = static_cast<QKeyEvent*>(ev);
        const bool window = obj->isWindowType();
        switch (k->key()) {
        case Qt::Key_VolumeUp: if (window) { step(+1); Q_EMIT keyUsed(); } return true;
        case Qt::Key_VolumeDown: if (window) { step(-1); Q_EMIT keyUsed(); } return true;
        case Qt::Key_VolumeMute: if (window && !k->isAutoRepeat()) { toggleMute(); Q_EMIT keyUsed(); } return true;
        default: break;
        }
    }
    return QObject::eventFilter(obj, ev);
}

// 実行中でなければすぐ始める。続けて同じ読み出しが並ぶときは 1 つにまとめる(結果は同じ)
void SystemVolume::enqueue(QStringList args) {
    if (args == kGet && !queue_.empty() && queue_.back() == kGet) return;
    queue_.push_back(std::move(args));
    if (proc_.state() == QProcess::NotRunning) run_next();
}

void SystemVolume::run_next() {
    if (queue_.empty()) return;
    const QStringList args = queue_.front();
    queue_.pop_front();
    reading_ = args == kGet;
    proc_.setArguments(args);
    proc_.start();
}

void SystemVolume::finished(int code, QProcess::ExitStatus status) {
    const QString out = QString::fromUtf8(proc_.readAll()).trimmed();
    const bool ok = status == QProcess::NormalExit && code == 0;
    if (reading_) {
        // 例: "Volume: 0.45" / "Volume: 0.45 [MUTED]"
        static const QRegularExpression re(QStringLiteral("Volume:\\s*([0-9.]+)(\\s*\\[MUTED\\])?"));
        const auto m = re.match(out);
        if (ok && m.hasMatch()) {
            volume_ = m.captured(1).toDouble();
            muted_ = m.hasCaptured(2);
            available_ = true;
            error_.clear();
        } else {
            available_ = false;
            error_ = out.isEmpty() ? QStringLiteral("wpctl failed") : out.left(120);
        }
        Q_EMIT changed();
    } else if (!ok) {
        error_ = out.isEmpty() ? QStringLiteral("wpctl failed") : out.left(120);
        Q_EMIT changed();
    }
    run_next();
}

} // namespace spear::gui
