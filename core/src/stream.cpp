#include "spear/core/stream.hpp"

namespace spear {

std::string_view to_string(DeliveryPolicy p) {
    return p == DeliveryPolicy::Lossless ? "lossless" : "latest_only";
}

// ---- Subscription ------------------------------------------------------------

Subscription::Subscription(std::string name, DeliveryPolicy p, std::size_t cap,
                           std::string stream_id, EventBus* ev)
    : name_(std::move(name)), policy_(p), capacity_(cap == 0 ? 1 : cap),
      stream_id_(std::move(stream_id)), events_(ev) {
    st_.name = name_;
    st_.policy = policy_;
    st_.queue_capacity = capacity_;
}

void Subscription::push(const BlockRef& b) {
    const auto& h = b.header();
    std::lock_guard lk(mu_);
    if (q_.size() >= capacity_) {
        if (policy_ == DeliveryPolicy::LatestOnly) {
            // 古いものを捨てて最新を優先。捨てた数は統計へ。欠落の直後の block に Discontinuity。
            st_.dropped_blocks++;
            st_.dropped_samples += q_.front().block.header().sample_count;
            q_.pop_front();
            if (!q_.empty()) q_.front().flags.set(Flag::Discontinuity);
            else pending_flags_.set(Flag::Discontinuity);
        } else {
            // Lossless: 入ってきた block を捨てる。ただし無言ではない。
            st_.dropped_blocks++;
            st_.dropped_samples += h.sample_count;
            pending_flags_.set(Flag::Discontinuity);
            if (!overflowing_) {
                overflowing_ = true;
                st_.overflow_bursts++;
                dropped_range_ = b.range();
                if (events_)
                    events_->emit(EventKind::ConsumerOverflow, stream_id_ + "/" + name_,
                                  "lossless queue full", dropped_range_, 0);
            } else if (h.generation == dropped_range_.generation) {
                dropped_range_.end = h.sample_end();
            }
            return;
        }
    }
    if (overflowing_) {
        // burst 終了: 捨てた範囲を Discontinuity event として確定
        overflowing_ = false;
        if (events_)
            events_->emit(EventKind::Discontinuity, stream_id_ + "/" + name_,
                          "samples dropped by lossless consumer", dropped_range_,
                          static_cast<int64_t>(dropped_range_.end - dropped_range_.begin));
    }
    q_.push_back(Delivery{b, h.flags | pending_flags_});
    pending_flags_ = {};
    if (q_.size() > st_.max_depth) st_.max_depth = q_.size();
    cv_.notify_one();
}

void Subscription::end() {
    std::lock_guard lk(mu_);
    eos_ = true;
    cv_.notify_all();
}

std::optional<Delivery> Subscription::take_locked() {
    if (q_.empty()) return std::nullopt;
    Delivery d = std::move(q_.front());
    q_.pop_front();
    st_.delivered_blocks++;
    st_.delivered_samples += d.block.header().sample_count;
    return d;
}

std::optional<Delivery> Subscription::pop(std::chrono::milliseconds timeout) {
    std::unique_lock lk(mu_);
    cv_.wait_for(lk, timeout, [this] { return !q_.empty() || eos_; });
    return take_locked();
}

std::optional<Delivery> Subscription::try_pop() {
    std::lock_guard lk(mu_);
    return take_locked();
}

bool Subscription::eos() const {
    std::lock_guard lk(mu_);
    return eos_ && q_.empty();
}

ConsumerStats Subscription::stats() const {
    std::lock_guard lk(mu_);
    ConsumerStats s = st_;
    s.queue_depth = q_.size();
    return s;
}

// ---- StreamBase ---------------------------------------------------------------

StreamBase::StreamBase(StreamMeta meta, EventBus* events)
    : meta_(std::move(meta)), events_(events) {}

StreamBase::~StreamBase() = default;

std::shared_ptr<Subscription> StreamBase::subscribe(std::string name, DeliveryPolicy policy,
                                                    std::size_t capacity) {
    std::shared_ptr<Subscription> s(new Subscription(std::move(name), policy, capacity, meta_.id, events_));
    std::lock_guard lk(mu_);
    std::erase_if(subs_, [](auto& w) { return w.expired(); });
    subs_.push_back(s);
    if (ended_) s->end();
    return s;
}

void StreamBase::publish(const BlockRef& block) {
    // subscriber list の snapshot を取り、lock 外で push する(subscribe と publish が競合しない)。
    std::vector<std::shared_ptr<Subscription>> live;
    {
        std::lock_guard lk(mu_);
        ++published_;
        live.reserve(subs_.size());
        bool prune = false;
        for (auto& w : subs_) {
            if (auto s = w.lock()) live.push_back(std::move(s));
            else prune = true;
        }
        if (prune) std::erase_if(subs_, [](auto& w) { return w.expired(); });
    }
    for (auto& s : live) s->push(block);
}

void StreamBase::end() {
    std::vector<std::shared_ptr<Subscription>> live;
    {
        std::lock_guard lk(mu_);
        ended_ = true;
        for (auto& w : subs_) if (auto s = w.lock()) live.push_back(std::move(s));
    }
    for (auto& s : live) s->end();
}

bool StreamBase::ended() const {
    std::lock_guard lk(mu_);
    return ended_;
}

std::vector<ConsumerStats> StreamBase::stats() const {
    std::vector<ConsumerStats> out;
    std::lock_guard lk(mu_);
    for (auto& w : subs_) if (auto s = w.lock()) out.push_back(s->stats());
    return out;
}

std::size_t StreamBase::consumer_count() const {
    std::lock_guard lk(mu_);
    std::size_t n = 0;
    for (auto& w : subs_) if (!w.expired()) ++n;
    return n;
}

uint64_t StreamBase::published_blocks() const {
    std::lock_guard lk(mu_);
    return published_;
}

} // namespace spear
