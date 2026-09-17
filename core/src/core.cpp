#include "spear/core/core.hpp"

#include <chrono>
#include <thread>

namespace spear {

Core::Core(std::string disk_path)
    : health_(std::make_unique<HealthMonitor>(&events_, std::move(disk_path))) {}

Core::~Core() {
    stop_app();
    if (source_) source_->stop();
    source_.reset();
    health_.reset();
}

bool Core::set_source(std::unique_ptr<Source> source) {
    if (active_app()) return false;
    if (source_) source_->stop();
    source_ = std::move(source);
    return true;
}

bool Core::run_app(std::shared_ptr<App> app, std::string* err) {
    stop_app();
    if (!source_) { if (err) *err = "no source"; return false; }
    // 宣言をそのまま適用。適用不能なら App 起動を失敗させる (§8.1)。
    const RfConfig cfg = app->declare_rf_config();
    if (!source_->configure(cfg, err)) {
        events_.emit(EventKind::Error, "core", "rf config rejected for app " + app->name() + (err ? ": " + *err : ""));
        return false;
    }
    {
        std::lock_guard lk(mu_);
        app_ = app;
    }
    events_.emit(EventKind::Info, "core", "app start: " + app->name());
    app->start(*this);   // App が subscribe してから
    source_->start();    // stream を流す

    // 宣言の適用結果を待つ: Streaming → OK / Fault(stage 2〜4 = 構成起因) → 起動失敗
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(ready_timeout_s_));
    while (std::chrono::steady_clock::now() < deadline) {
        const auto ds = source_->device_status();
        if (ds.state == DeviceState::Streaming) return true;
        if (ds.state == DeviceState::Fault && ds.stage >= 2 && ds.stage <= 4) {
            if (err) *err = ds.evidence;
            events_.emit(EventKind::Error, "core", "app " + app->name() + " rejected: " + ds.evidence);
            stop_app();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true; // 装置待ち(不在 / FPGA ロード中 / 再接続中)。App は状態を表示しながら待つ
}

void Core::stop_app() {
    std::shared_ptr<App> app;
    {
        std::lock_guard lk(mu_);
        app.swap(app_);
    }
    if (!app) return;
    source_->stop();
    app->stop();
    events_.emit(EventKind::Info, "core", "app stop: " + app->name());
}

std::shared_ptr<App> Core::active_app() const {
    std::lock_guard lk(mu_);
    return app_;
}

} // namespace spear
