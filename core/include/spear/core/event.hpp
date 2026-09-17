// S.P.E.A.R. core — Event (要件 §4.1, §12.1)
//
// 離散事象。全 Event は「自分の元となった sample index 範囲」を持つ (§4.5)。
// 発行側スレッド(RX thread 等)を listener が遅延させないよう、配送は専用 thread で行う。
// 最近の履歴を ring に残し、装置画面上で診断を完結させる (§12.1)。
#pragma once

#include "types.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spear {

enum class EventKind : uint8_t {
    // Stream Bus
    ConsumerOverflow,   // Lossless consumer のキュー溢れ (§10)
    ConsumerDrop,       // LatestOnly consumer の破棄(集約して報告)
    // Radio / Core health (§12.1)
    StartupStage,       // 起動シーケンス各段の結果 (§17.2)
    Overflow,           // ERROR_CODE_OVERFLOW
    OutOfSequence,      // transport packet loss
    Timeout,            // recv timeout
    Discontinuity,      // sample index 不連続
    Underflow,          // TX
    Retune,
    Disconnected,
    Reconnected,        // generation 更新を伴う
    ClockReset,
    ThermalThrottle,
    DiskLow,
    DeviceState,        // USRP 状態遷移。value = DeviceState, detail = evidence
    UhdLog,             // UHD 自身のログ(warning 以上、または B200 の info)。detail = message
    TimeReference,      // 時刻基準の記録 (§4.4)。range.generation/begin = 対応点, value = utc_ns, detail = "hw=<s> mono=<ns>"
    LoUnlock,           // 受信中に lo_locked が false になった / 復帰した(value: 0=unlock, 1=relock)
    // App
    Info,
    Warning,
    Error,
};
std::string_view to_string(EventKind k);

struct Event {
    EventKind   kind = EventKind::Info;
    std::string source;      // stream ID / component 名
    SampleRange range;       // 元となった sample index 範囲 (無ければ empty)
    uint64_t    host_ns = 0; // steady_clock
    std::string detail;
    int64_t     value = 0;   // 汎用数値(drop 数、段番号など)
};

class EventBus {
public:
    using Listener = std::function<void(const Event&)>;

    explicit EventBus(std::size_t history = 512);
    ~EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // 任意スレッドから呼べる。ブロックしない(履歴 ring への追記 + queue)。
    void emit(Event ev);
    void emit(EventKind k, std::string source, std::string detail = {},
              SampleRange range = {}, int64_t value = 0);

    // listener は dispatch thread 上で呼ばれる。登録解除は id で。
    int  subscribe(Listener l);
    void unsubscribe(int id);

    std::vector<Event> history() const;         // 新しい順
    uint64_t count(EventKind k) const;
    void flush();                               // queue が空になるまで待つ(テスト用)

private:
    void run();

    const std::size_t history_cap_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Event> queue_;
    std::deque<Event> history_;
    std::vector<std::pair<int, Listener>> listeners_;
    std::vector<uint64_t> counts_;
    int next_id_ = 1;
    bool stop_ = false;
    std::size_t in_flight_ = 0;
    std::thread th_;
};

uint64_t host_now_ns();

} // namespace spear
