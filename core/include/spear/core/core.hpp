// S.P.E.A.R. core — Core (要件 §2)
//
// 責務は3つだけ: Radio(Source)を所有する / App の宣言に従い tune する / Stream Bus で fan-out する。
// GUI が無くても完全に動作する (§11)。
#pragma once

#include "app.hpp"
#include "event.hpp"
#include "health.hpp"
#include "source.hpp"

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

    // source は差し替え可能 (§7): B210LiveSource / RecordingSource / SyntheticSource。
    // Source は events() を参照して構築する。App 実行中の差し替えは不可(false)。
    bool set_source(std::unique_ptr<Source> source);
    Source&        source()  { return *source_; }
    bool           has_source() const { return static_cast<bool>(source_); }
    Stream<sc16>&  rx()      { return source_->output(); }

    // active App は常に1つ (§3.2)。前の App があれば stop してから切り替える。
    // RF 宣言が装置に適用不能(範囲外・coercion・link 帯域超過 = Fault at stage 2〜4)なら
    // App を止めて false を返す (§8.1)。装置不在・FPGA ロード中などの「待ち」は失敗ではなく、
    // App は起動したまま DeviceState を表示する。判定は ready_timeout_s まで待つ。
    bool run_app(std::shared_ptr<App> app, std::string* err = nullptr);
    void set_ready_timeout(double s) { ready_timeout_s_ = s; }
    void stop_app();
    std::shared_ptr<App> active_app() const;

private:
    EventBus events_;
    std::unique_ptr<HealthMonitor> health_;
    std::unique_ptr<Source> source_;
    mutable std::mutex mu_;
    std::shared_ptr<App> app_;
    double ready_timeout_s_ = 10.0;
};

} // namespace spear
