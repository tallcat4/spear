#include "spear/core/core.hpp"

#include <chrono>
#include <thread>

namespace spear {

Core::Core(std::string disk_path)
    : hold_(std::make_unique<PerformanceHold>(&events_)), health_(std::make_unique<HealthMonitor>(&events_, std::move(disk_path))) {
    health_->set_observer([this](const HealthSnapshot& s) { hold_->check(s.epp); });
}

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
    if (source_) source_->set_tx_arm(&tx_armed_);
    return true;
}

bool Core::run_app(std::shared_ptr<App> app, std::string* err) {
    stop_app();
    if (!source_) { if (err) *err = "no source"; return false; }
    // 宣言をそのまま適用。適用不能なら App 起動を失敗させる (§7.1)。
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
    if (hold_enabled_) hold_->acquire("S.P.E.A.R. " + app->name());   // DSP が追いつくように(§11.1)。失敗しても起動は続ける
    app->start(*this);   // App が subscribe してから
    source_->start();    // stream を流す

    // 宣言の適用結果を待つ: Streaming → OK / Fault(stage 2〜4 = 構成起因) → 起動失敗
    if (!wait_applied(err)) {
        events_.emit(EventKind::Error, "core", "app " + app->name() + " rejected: " + (err ? *err : std::string()));
        stop_app();
        return false;
    }
    return true; // Streaming、または装置待ち(不在 / FPGA ロード中 / 再接続中)。App は状態を表示しながら待つ
}

bool Core::wait_applied(std::string* err) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(ready_timeout_s_));
    while (std::chrono::steady_clock::now() < deadline) {
        const auto ds = source_->device_status();
        if (ds.state == DeviceState::Streaming) return true;
        if (ds.state == DeviceState::Fault && ds.stage >= 2 && ds.stage <= 4) {
            if (err) *err = ds.evidence;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

bool Core::reconfigure(const std::shared_ptr<App>& app, const RfConfig& cfg, std::string* err) {
    if (!app || active_app() != app) { if (err) *err = "app is not active"; return false; }
    const RfConfig before = source_->config();
    // 1 回の構成し直し: Source を止め、App に stream を手放させ、構成し、App に付け直させてから流す
    auto apply = [&](const RfConfig& c, std::string* e) {
        source_->stop();
        app->rf_will_change();
        const bool ok = source_->configure(c, e);
        app->rf_changed(*this);
        source_->start();
        return ok && wait_applied(e);
    };
    events_.emit(EventKind::Info, "core", "app reconfigure: " + app->name());
    std::string e;
    // 宣言の時点で受け付けられない(configure が拒否 = Source の宣言は元のまま)なら、元の構成で 1 回だけ流し直す
    source_->stop();
    app->rf_will_change();
    if (!source_->configure(cfg, &e)) {
        app->rf_changed(*this);
        source_->start();
        if (err) *err = e;
        events_.emit(EventKind::Error, "core", "reconfigure rejected for app " + app->name() + ": " + e + " (previous configuration kept)");
        wait_applied(nullptr);
        return false;
    }
    app->rf_changed(*this);
    source_->start();
    if (wait_applied(&e)) return true;
    // 装置が適用できなかった(範囲外・coercion など、stage 2〜4 の Fault): 元の構成に戻す
    if (err) *err = e;
    events_.emit(EventKind::Error, "core", "reconfigure rejected for app " + app->name() + ": " + e + " (previous configuration restored)");
    std::string e2;
    if (!apply(before, &e2)) events_.emit(EventKind::Error, "core", "previous configuration also failed: " + e2);
    return false;
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
    hold_->release();
    events_.emit(EventKind::Info, "core", "app stop: " + app->name());
}

void Core::set_tx_armed(bool armed) {
    if (tx_armed_.exchange(armed) == armed) return;
    events_.emit(armed ? EventKind::Warning : EventKind::Info, "core", armed ? "TX MASTER: ARMED (transmission allowed)" : "TX MASTER: SAFE (all transmission refused)");
}

std::shared_ptr<App> Core::active_app() const {
    std::lock_guard lk(mu_);
    return app_;
}

} // namespace spear
