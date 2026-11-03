// S.P.E.A.R. core — App 実行中の CPU 性能の要求 (要件 §11.1)
//
// FZ-G2 のバッテリー駆動では、EPP が performance 以外(balance_power / balance_performance、数値 96 以上)だと
// spear の受信中の負荷(8.5 ms ごとの短い処理が 8 論理コアに散る)に HWP が 900 MHz を選び続け、DSP が追いつかなくなる
// (2026-11-03 の測定: JR TRAIN 22 波で DSP 55 % → performance で 11 %)。中間の EPP で電力を抑えられる値は無かった。
// そこで App の実行中だけ power-profiles-daemon に performance を hold する(D-Bus HoldProfile)。
//   - hold は接続に結びつく: プロセスが落ちても ppd が解除する。接続は hold の間ずっと持つ
//   - polkit は hold-profile を allow_active(アクティブなセッションの中でだけ許可)。Plasma でもキオスクセッションでも通る
//   - ppd は誰かが SetProfile すると全 hold を外す(Plasma の電源設定が AC/バッテリーの切り替えで呼ぶ)。
//     check() を HealthMonitor の周期で呼び、要求中なのに EPP が performance でなければ掛け直す(event を出す)
// 失敗(ppd なし・polkit 拒否・timeout)は Warning event を 1 回出して続ける。App の起動は止めない。
#pragma once

#include "event.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

struct sd_bus;

namespace spear {

class PerformanceHold {
public:
    explicit PerformanceHold(EventBus* events);
    ~PerformanceHold();
    PerformanceHold(const PerformanceHold&) = delete;
    PerformanceHold& operator=(const PerformanceHold&) = delete;

    void acquire(const std::string& reason);   // 要求する(既に持っていれば何もしない)
    void release();                            // 要求をやめる
    void check(const std::string& epp);        // 要求中に EPP が performance でなければ掛け直す(HealthMonitor の周期で呼ぶ)
    bool wanted() const;
    bool held() const;                         // ppd が cookie を返した(外されたかどうかは EPP で見る)

private:
    bool hold_locked();
    void drop_locked(bool report = true);
    EventBus* events_;
    mutable std::mutex mu_;
    sd_bus* bus_ = nullptr;
    std::optional<uint32_t> cookie_;
    std::string reason_;
    bool wanted_ = false;
    bool warned_ = false;
    bool mismatch_reported_ = false;   // 要求中に EPP が performance でないことを event にした(performance に戻るまで繰り返さない)
    std::chrono::steady_clock::time_point last_try_{};
};

} // namespace spear
