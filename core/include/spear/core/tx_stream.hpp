// S.P.E.A.R. core — TX の fan-in(要件 §4.1, §4.3, §6)
//
// 送信側は producer(App)が 1 つ、consumer(Source の送信 thread = Radio)が 1 つ(§4.1「fan-out より単純」)。
// RX の Stream<T> とは不変条件の向きが逆になる:
//   * RX Lossless: producer(装置)は止められないので、溢れたら捨てて Discontinuity を報告する。
//   * TX:          producer(App)は待たせられるので、キューが一杯なら write() が空くまで待つ(捨てない)。
//                  欠落が起きうるのは装置側の書き遅れ(underflow)だけで、それは必ず Underflow event にする(§4.3)。
// burst の境界は block の flags(StartOfBurst / EndOfBurst)で表す。EndOfBurst は 0 sample の block でもよい。
// close() で producer の待ちは解け、以後の write() は false を返す(Source::stop → App::stop の順で止められる)。
// close() はキューに残った block を捨てる(止めるときだけ)。block は producer の BlockPool を指すので、producer は
// pool を Source が止まるまで生かす(App のメンバにして on_stop で破棄する。Core::stop_app は Source を先に止める)。
//
// マスターアーム: 送信できるのは Core の TX MASTER が ARMED のときだけ(所有者は Core、Core::set_tx_armed)。
// SAFE なら write() は拒否し(false、event を 1 回)、consumer は burst を始めない。送信中に SAFE になったら残りを捨て、
// 送信 thread は EOB を送って burst を閉じる。どの App から来た送信もこの入口を通るので、ここで止める。
#pragma once

#include <atomic>

#include "block.hpp"
#include "event.hpp"
#include "types.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace spear {

inline constexpr const char* kRadioTxStreamId = "radio.tx";
inline constexpr std::size_t kDefaultTxQueueBlocks = 32;   // 16384 sample/block で 10 Msps なら ~52 ms 分

struct TxStats {
    uint64_t written_blocks  = 0;   // producer が入れた block
    uint64_t written_samples = 0;
    uint64_t sent_blocks     = 0;   // consumer(装置)へ渡した block
    uint64_t sent_samples    = 0;
    uint64_t bursts          = 0;   // 始まった burst の数
    uint64_t underflows      = 0;   // 装置の書き遅れ(B210 は async msg、装置なしは consumer の空振り)
    uint64_t seq_errors      = 0;   // B210: EVENT_CODE_SEQ_ERROR(USB 上のパケット欠落)
    uint64_t time_errors     = 0;   // B210: EVENT_CODE_TIME_ERROR(遅れた timed burst)
    bool        in_burst     = false;   // 装置が今 burst の中にいる(SOB を送って EOB をまだ送っていない)= 送信中
    std::size_t queue_depth  = 0;
    std::size_t queue_capacity = 0;
    std::size_t max_depth    = 0;
};

class TxStream {
public:
    // arm: Core の TX MASTER(null なら常に SAFE)
    TxStream(StreamMeta meta, EventBus* events, const std::atomic<bool>* arm, std::size_t capacity = kDefaultTxQueueBlocks);
    TxStream(const TxStream&) = delete;
    TxStream& operator=(const TxStream&) = delete;

    const StreamMeta& meta() const noexcept { return meta_; }

    // ---- producer(App の thread)----
    // キューに空きができるまで待って入れる。false は閉じられたときだけ(捨てることはない)。
    // App は Source より先に始まり、装置の open(数秒)の間もここで待つので、時間で諦めてはならない
    bool write(const BlockRef& block);
    bool armed() const { return arm_ && arm_->load(); }
    // 待つ時間を限る版(テスト用)。空かなければ false
    bool try_write(const BlockRef& block, std::chrono::milliseconds timeout);
    bool closed() const;

    // ---- consumer(Source の送信 thread)----
    // SAFE の間はキューを捨てて何も返さない(送信 thread は in_burst なら armed() を見て burst を閉じる)
    std::optional<BlockRef> pop(std::chrono::milliseconds timeout);
    void close();                       // producer の待ちを解く。以後 write は false
    // 送信 thread が装置の状態を記録する(stats() に出る。SystemModel が「送信中」を Core から読む)
    void note_sent(const BlockRef& b);
    void note_burst(bool in_burst);
    // count 回の書き遅れ / 異常をまとめて 1 つの event にする(装置の報告が続けて来ても event log を押し流さない)。value = count
    void note_underflow(uint64_t count, SampleRange where, const std::string& source, const std::string& detail);
    void note_error(EventKind kind, bool seq, uint64_t count, SampleRange where, const std::string& source, const std::string& detail);

    TxStats stats() const;

private:
    bool push_locked(const BlockRef& block);
    StreamMeta meta_;
    EventBus* events_;
    const std::atomic<bool>* arm_;
    bool refusing_ = false;   // SAFE で拒否中(event は拒否が始まったときに 1 回)
    const std::size_t capacity_;
    mutable std::mutex mu_;
    std::condition_variable cv_space_, cv_data_;
    std::deque<BlockRef> q_;
    bool closed_ = false;
    TxStats st_;
};

} // namespace spear
