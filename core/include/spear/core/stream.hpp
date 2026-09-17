// S.P.E.A.R. core — Stream Bus (要件 §5, §10)
//
// 静的配線された fan-out 層。registry も discovery も動的 attach も持たない (§3.1)。
//
// 不変原則 (§5.2): Producer は consumer の具体的な存在を知らない。
//   producer は Stream::publish() を呼ぶだけ。誰が subscribe しているかは知らないし知れない。
//
// 不変条件 (§10): RX Lossless stream は、全 sample を配送するか discontinuity を発行するかの
//   いずれかである。無言の欠落は存在しない。
//   → Lossless consumer のキューが溢れた場合、producer は block を捨てる(止まらない)が、
//     (a) ConsumerOverflow event を発行し、(b) 次に配送する block に Discontinuity flag を立て、
//     (c) 捨てた sample 数を統計に残す。
//   → LatestOnly consumer は古い block を捨て、破棄数を公開する。
//
// consumer が遅くても producer も他 consumer も影響を受けない: push は consumer ごとの
// mutex を一瞬保持するだけで、consumer 側が長く保持する経路は無い。
#pragma once

#include "block.hpp"
#include "event.hpp"
#include "types.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace spear {

enum class DeliveryPolicy : uint8_t {
    Lossless,   // decoder, IQ Recorder, TX stream
    LatestOnly, // waterfall, spectrum, GUI 表示全般
};
std::string_view to_string(DeliveryPolicy p);

struct ConsumerStats {
    std::string    name;
    DeliveryPolicy policy = DeliveryPolicy::Lossless;
    uint64_t delivered_blocks  = 0;
    uint64_t delivered_samples = 0;
    uint64_t dropped_blocks    = 0;
    uint64_t dropped_samples   = 0;
    uint64_t overflow_bursts   = 0; // Lossless: 溢れの回数(burst 単位)
    std::size_t queue_depth    = 0;
    std::size_t queue_capacity = 0;
    std::size_t max_depth      = 0; // 観測された最大 depth(容量に対する余裕の指標)
};

// consumer が受け取る単位。flags には block 自身の flags に加え、
// この consumer 固有の Discontinuity(自分のキュー溢れ由来)が合成される。
struct Delivery {
    BlockRef block;
    Flags    flags;
};

class StreamBase;

// consumer 側 handle。破棄すれば detach(producer 側コードは変わらない)。
class Subscription {
public:
    ~Subscription() = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    std::optional<Delivery> pop(std::chrono::milliseconds timeout);
    std::optional<Delivery> try_pop();
    bool eos() const;                      // producer が end() し、キューも空
    ConsumerStats stats() const;
    const std::string& name() const noexcept { return name_; }
    DeliveryPolicy policy() const noexcept { return policy_; }

private:
    friend class StreamBase;
    Subscription(std::string name, DeliveryPolicy p, std::size_t cap, std::string stream_id, EventBus* ev);
    void push(const BlockRef& b);          // producer thread から
    void end();
    std::optional<Delivery> take_locked();

    const std::string    name_;
    const DeliveryPolicy policy_;
    const std::size_t    capacity_;
    const std::string    stream_id_;
    EventBus* const      events_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Delivery> q_;               // flags: この consumer から見た合成済み flag
    bool eos_ = false;
    bool overflowing_ = false;
    Flags pending_flags_;                  // 欠落の「後」に来る最初の block へ合成する flag
    SampleRange dropped_range_;            // 現在の burst で捨てた範囲
    ConsumerStats st_;
};

// 型を持たない共通部。利用側は Stream<T> を使う。
class StreamBase {
public:
    StreamBase(StreamMeta meta, EventBus* events);
    virtual ~StreamBase();
    StreamBase(const StreamBase&) = delete;
    StreamBase& operator=(const StreamBase&) = delete;

    const StreamMeta& meta() const noexcept { return meta_; }

    // consumer 追加。capacity は block 数。LatestOnly は通常 1〜2 で足りる。
    std::shared_ptr<Subscription> subscribe(std::string name, DeliveryPolicy policy, std::size_t capacity);

    // producer: block を全 consumer へ配布(参照のみ、コピーなし)。ブロックしない。
    void publish(const BlockRef& block);

    // lifecycle (§5.1)
    void end();                            // EOS を全 consumer へ
    bool ended() const;

    std::vector<ConsumerStats> stats() const;
    std::size_t consumer_count() const;
    uint64_t published_blocks() const;

protected:
    StreamMeta meta_;
    EventBus*  events_;

private:
    mutable std::mutex mu_;
    std::vector<std::weak_ptr<Subscription>> subs_;
    bool ended_ = false;
    uint64_t published_ = 0;
};

template <class T>
class Stream : public StreamBase {
public:
    using sample_type = T;
    Stream(StreamMeta meta, EventBus* events) : StreamBase(std::move(meta), events) {
        meta_.dtype = dtype_of<T>();
    }
    void publish(const BlockRef& block) {
        assert(block.header().dtype == dtype_of<T>());
        StreamBase::publish(block);
    }
};

// consumer 側で sample index の連続性を検査する補助 (§4.5)。
// generation が変われば比較せず「新しい時間軸」として受け入れる (§4.4.1)。
class ContinuityChecker {
public:
    // 欠落があれば {generation, expected, actual} を返す(actual > expected)。
    struct Gap { uint64_t generation, expected, actual; };
    std::optional<Gap> check(const BlockHeader& h) {
        std::optional<Gap> gap;
        if (have_ && h.generation == gen_ && h.sample_index != next_)
            gap = Gap{gen_, next_, h.sample_index};
        have_ = true;
        gen_  = h.generation;
        next_ = h.sample_end();
        return gap;
    }
private:
    bool have_ = false;
    uint64_t gen_ = 0, next_ = 0;
};

} // namespace spear
