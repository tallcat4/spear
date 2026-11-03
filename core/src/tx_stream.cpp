#include "spear/core/tx_stream.hpp"

namespace spear {

TxStream::TxStream(StreamMeta meta, EventBus* events, const std::atomic<bool>* arm, std::size_t capacity)
    : meta_(std::move(meta)), events_(events), arm_(arm), capacity_(capacity == 0 ? 1 : capacity) {
    meta_.dtype = DataType::ComplexInt16;
    meta_.direction = Direction::TX;
    st_.queue_capacity = capacity_;
}

bool TxStream::write(const BlockRef& block) {
    std::unique_lock lk(mu_);
    cv_space_.wait(lk, [this] { return closed_ || q_.size() < capacity_; });
    return push_locked(block);
}

bool TxStream::try_write(const BlockRef& block, std::chrono::milliseconds timeout) {
    std::unique_lock lk(mu_);
    if (!cv_space_.wait_for(lk, timeout, [this] { return closed_ || q_.size() < capacity_; })) return false;
    return push_locked(block);
}

bool TxStream::push_locked(const BlockRef& block) {
    if (closed_) return false;
    if (!armed()) {
        if (!refusing_) {
            refusing_ = true;
            if (events_) events_->emit(EventKind::Warning, meta_.id, "tx refused: TX MASTER is SAFE (arm it in the main menu)", block.range());
        }
        return false;
    }
    refusing_ = false;
    q_.push_back(block);
    st_.written_blocks++;
    st_.written_samples += block.header().sample_count;
    if (q_.size() > st_.max_depth) st_.max_depth = q_.size();
    cv_data_.notify_one();
    return true;
}

bool TxStream::closed() const {
    std::lock_guard lk(mu_);
    return closed_;
}

std::optional<BlockRef> TxStream::pop(std::chrono::milliseconds timeout) {
    std::unique_lock lk(mu_);
    cv_data_.wait_for(lk, timeout, [this] { return closed_ || !q_.empty(); });
    if (!armed()) {
        if (!q_.empty()) { q_.clear(); cv_space_.notify_all(); }   // SAFE になった: 送らない(producer の次の write は拒否される)
        return std::nullopt;
    }
    if (q_.empty()) return std::nullopt;
    BlockRef b = std::move(q_.front());
    q_.pop_front();
    cv_space_.notify_one();
    return b;
}

void TxStream::close() {
    std::lock_guard lk(mu_);
    closed_ = true;
    q_.clear();   // 送らない block を手放す(block は producer の pool を指す。Source を止めた時点で参照を残さない)
    cv_space_.notify_all();
    cv_data_.notify_all();
}

void TxStream::note_sent(const BlockRef& b) {
    std::lock_guard lk(mu_);
    st_.sent_blocks++;
    st_.sent_samples += b.header().sample_count;
}

void TxStream::note_burst(bool in_burst) {
    std::lock_guard lk(mu_);
    if (in_burst && !st_.in_burst) st_.bursts++;
    st_.in_burst = in_burst;
}

void TxStream::note_underflow(uint64_t count, SampleRange where, const std::string& source, const std::string& detail) {
    if (count == 0) return;
    {
        std::lock_guard lk(mu_);
        st_.underflows += count;
    }
    if (events_) events_->emit(EventKind::Underflow, source, detail, where, static_cast<int64_t>(count));
}

void TxStream::note_error(EventKind kind, bool seq, uint64_t count, SampleRange where, const std::string& source, const std::string& detail) {
    if (count == 0) return;
    {
        std::lock_guard lk(mu_);
        if (seq) st_.seq_errors += count; else st_.time_errors += count;
    }
    if (events_) events_->emit(kind, source, detail, where, static_cast<int64_t>(count));
}

TxStats TxStream::stats() const {
    std::lock_guard lk(mu_);
    TxStats s = st_;
    s.queue_depth = q_.size();
    return s;
}

} // namespace spear
