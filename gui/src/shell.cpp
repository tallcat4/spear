#include "shell.hpp"

#include <QMetaObject>

namespace spear::gui {

Shell::Shell(Core& core, RfConfig draft, QObject* parent) : QObject(parent), core_(core), draft_(draft) {
    for (const auto& e : appfw::registered_apps()) {
        instances_.push_back(e.make());
        QVariantMap m;
        m["name"] = QString::fromStdString(e.info.name);
        m["description"] = QString::fromStdString(e.info.description);
        m["direction"] = e.info.direction == Direction::RX ? "RX" : "TX";
        apps_.push_back(m);
    }
    QVariantMap d;
    d["name"] = "DIAGNOSTICS"; d["description"] = "Device state, stream bus, host health, event log"; d["direction"] = "—";
    apps_.push_back(d);
}

Shell::~Shell() {
    if (worker_.joinable()) worker_.join();
}

QString Shell::activePage() const {
    if (active_ < 0 || active_ >= static_cast<int>(instances_.size())) return {};
    return QString::fromStdString(instances_[active_]->info().page_url);
}

void Shell::startApp(int index) {
    if (busy_) return;
    if (index == diagnosticsIndex()) { diagnostics_ = true; Q_EMIT activeChanged(); return; }
    if (index < 0 || index >= static_cast<int>(instances_.size())) return;
    // 立ち上げ(FPGA ロード等)の途中では App を始めない。start() は完了を待つが、GUI を 1 分止めるより拒否して見せる
    if (core_.source().warming_up()) { last_error_ = "device is still warming up"; Q_EMIT activeChanged(); return; }
    auto app = instances_[index];
    app->set_rf_config(draft_);
    busy_ = true;
    last_error_.clear();
    Q_EMIT activeChanged();
    if (worker_.joinable()) worker_.join();
    // run_app は装置の適用結果を待つので GUI thread では呼ばない
    worker_ = std::thread([this, app, index] {
        std::string err;
        const bool ok = core_.run_app(app, &err);
        QMetaObject::invokeMethod(this, [this, ok, err, index] {
            busy_ = false;
            active_ = ok ? index : -1;
            last_error_ = ok ? QString() : QString::fromStdString(err);
            Q_EMIT activeChanged();
        }, Qt::QueuedConnection);
    });
}

void Shell::stopApp() {
    if (busy_) return;
    if (diagnostics_) { diagnostics_ = false; Q_EMIT activeChanged(); if (active_ >= 0) return; }
    if (active_ < 0) return;
    busy_ = true;
    Q_EMIT activeChanged();
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this] {
        core_.stop_app();
        QMetaObject::invokeMethod(this, [this] {
            busy_ = false;
            active_ = -1;
            // 草案は「最後に Core が運転していた RF」に追従する(次の汎用 App は前回の周波数・レートから始まる)。
            // 所有者は Core。ここは起動時に渡す写しにすぎない。
            draft_ = core_.source().config();
            Q_EMIT draftChanged();
            Q_EMIT activeChanged();
            if (restart_index_ >= 0) { const int i = restart_index_; restart_index_ = -1; draft_.sample_rate = pending_rate_; startApp(i); }
        }, Qt::QueuedConnection);
    });
}

void Shell::restartActiveWithRate(double sample_rate) {
    if (busy_ || active_ < 0) return;
    restart_index_ = active_;
    pending_rate_ = sample_rate;
    stopApp();
}

} // namespace spear::gui
