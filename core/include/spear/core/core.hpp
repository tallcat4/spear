// S.P.E.A.R. core — Core (要件 §2.1)
//
// 責務は3つだけ: Radio(Source)を所有する / App の宣言に従い tune する / Stream Bus で fan-out する。
// GUI が無くても完全に動作する (§10.4)。
#pragma once

#include "app.hpp"
#include "event.hpp"
#include "health.hpp"
#include "power_hold.hpp"
#include "source.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace spear {

class Core {
public:
    explicit Core(std::string disk_path = ".");
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    EventBus&      events()  { return events_; }
    HealthMonitor& health()  { return *health_; }

    // source は差し替え可能 (§6): B210LiveSource / RecordingSource / SyntheticSource。
    // Source は events() を参照して構築する。App 実行中の差し替えは不可(false)。
    bool set_source(std::unique_ptr<Source> source);
    Source&        source()  { return *source_; }
    bool           has_source() const { return static_cast<bool>(source_); }
    Stream<sc16>&  rx()      { return source_->output(); }
    TxStream&      tx()      { return source_->tx_input(); }   // 送信の入口(fan-in)。direction == TX の App が on_start で受け取る

    // active App は常に1つ (§2.3)。前の App があれば stop してから切り替える。
    // RF 宣言が装置に適用不能(範囲外・coercion・link 帯域超過 = Fault at stage 2〜4)なら
    // App を止めて false を返す (§7.1)。装置不在・FPGA ロード中などの「待ち」は失敗ではなく、
    // App は起動したまま DeviceState を表示する。判定は ready_timeout_s まで待つ。
    bool run_app(std::shared_ptr<App> app, std::string* err = nullptr);
    void set_ready_timeout(double s) { ready_timeout_s_ = s; }
    // App の実行中は CPU を performance にするよう power-profiles-daemon に要求する(§11.1、power_hold.hpp)。
    // 既定は off(テストや headless の道具が ppd に触らないように)。装置の GUI が on にする
    void set_performance_hold(bool on) { hold_enabled_ = on; }
    const PerformanceHold& performance_hold() const { return *hold_; }
    void stop_app();
    std::shared_ptr<App> active_app() const;
    // 運転中の App の RF を構成し直す(App は止めない。送信 App の録音選択、汎用 App のレート変更など)。
    // Source を止める → app->rf_will_change() → configure(cfg) → app->rf_changed() → Source を始める → 装置の適用を待つ(run_app と同じ判定)。
    // 適用できなければ元の構成に戻して続け、false(App は動いたまま)。app が active でなければ false
    bool reconfigure(const std::shared_ptr<App>& app, const RfConfig& cfg, std::string* err = nullptr);

    // ---- TX MASTER(マスターアーム)----
    // ARMED のときだけ送信できる。SAFE ならどの App の送信も Core の送信入口(TxStream)が拒否し、送信中なら直ちに burst を閉じる。
    // 起動時は必ず SAFE(保存しない)。変更は Info event("core")で残す
    void set_tx_armed(bool armed);
    bool tx_armed() const { return tx_armed_.load(); }

private:
    // 宣言の適用結果を待つ: Streaming → true / Fault(stage 2〜4 = 構成起因)→ false(err)/ 装置待ちのまま期限 → true
    bool wait_applied(std::string* err);
    EventBus events_;
    std::unique_ptr<PerformanceHold> hold_;   // health_ より先に作り、後に壊す(監視 thread が check() を呼ぶ)
    std::unique_ptr<HealthMonitor> health_;
    std::unique_ptr<Source> source_;
    mutable std::mutex mu_;
    std::shared_ptr<App> app_;
    double ready_timeout_s_ = 10.0;
    std::atomic<bool> hold_enabled_{false};
    std::atomic<bool> tx_armed_{false};
};

} // namespace spear
