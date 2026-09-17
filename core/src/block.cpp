#include "spear/core/block.hpp"

#include <algorithm>

namespace spear {

std::string_view to_string(DataType t) {
    switch (t) {
    case DataType::ComplexInt16:   return "ci16";
    case DataType::ComplexFloat32: return "cf32";
    case DataType::Float32:        return "rf32";
    case DataType::Int16:          return "ri16";
    case DataType::UInt8:          return "ru8";
    default:                       return "unknown";
    }
}

std::string_view to_string(DeviceState s) {
    switch (s) {
    case DeviceState::Unknown:      return "UNKNOWN";
    case DeviceState::Disconnected: return "DISCONNECTED";
    case DeviceState::NoFirmware:   return "NO_FIRMWARE";
    case DeviceState::Standby:      return "STANDBY";
    case DeviceState::Initializing: return "INITIALIZING";
    case DeviceState::Ready:        return "READY";
    case DeviceState::Streaming:    return "STREAMING";
    case DeviceState::Fault:        return "FAULT";
    case DeviceState::Lost:         return "LOST";
    }
    return "?";
}

std::string to_string(Flags f) {
    std::string s;
    auto add = [&](Flag fl, const char* n) { if (f.has(fl)) { if (!s.empty()) s += '|'; s += n; } };
    add(Flag::Discontinuity, "discontinuity");
    add(Flag::Overflow,      "overflow");
    add(Flag::Underflow,     "underflow");
    add(Flag::Timeout,       "timeout");
    add(Flag::OutOfSequence, "out_of_sequence");
    add(Flag::Retune,        "retune");
    add(Flag::ClockReset,    "clock_reset");
    add(Flag::StartOfBurst,  "sob");
    add(Flag::EndOfBurst,    "eob");
    add(Flag::EndOfStream,   "eos");
    return s.empty() ? "none" : s;
}

// ---- BlockPool ---------------------------------------------------------------

BlockPool::BlockPool(std::size_t slot_bytes, std::size_t slots)
    : slot_bytes_(slot_bytes), arena_(slot_bytes * slots), blocks_(slots) {
    free_.reserve(slots);
    for (std::size_t i = 0; i < slots; ++i) {
        blocks_[i].owner   = this;
        blocks_[i].slot    = static_cast<int64_t>(i);
        blocks_[i].storage = {arena_.data() + i * slot_bytes, slot_bytes};
        free_.push_back(static_cast<int64_t>(i));
    }
}

BlockPool::~BlockPool() {
    // 未返却 block が残っていれば contract 違反(BlockRef が pool より長生き)。
    assert(in_use_ == 0 && "BlockRef outlived its BlockPool");
}

BlockBuilder BlockPool::acquire(DataType dtype, std::size_t max_samples) {
    const std::size_t need = max_samples * dtype_size(dtype);
    detail::Block* b = nullptr;
    {
        std::lock_guard lk(mu_);
        if (need <= slot_bytes_ && !free_.empty()) {
            b = &blocks_[static_cast<std::size_t>(free_.back())];
            free_.pop_back();
        } else {
            ++heap_fallbacks_;
            b = new detail::Block{};
            b->owner = this;
            b->slot  = -1;
            b->heap  = std::make_unique<std::byte[]>(need);
            b->storage = {b->heap.get(), need};
        }
        ++in_use_;
        peak_ = std::max(peak_, in_use_);
    }
    b->header = BlockHeader{};
    b->header.dtype = dtype;
    return BlockBuilder{b};
}

void BlockPool::release(detail::Block* b) noexcept {
    std::lock_guard lk(mu_);
    --in_use_;
    if (b->slot >= 0) free_.push_back(b->slot);
    else              delete b;
}

PoolStats BlockPool::stats() const {
    std::lock_guard lk(mu_);
    return {blocks_.size(), slot_bytes_, in_use_, peak_, heap_fallbacks_};
}

// ---- BlockBuilder -----------------------------------------------------------

BlockBuilder::~BlockBuilder() {
    if (b_) b_->owner->release(b_);
}

BlockRef BlockBuilder::commit(uint32_t sample_count) noexcept {
    assert(b_);
    assert(sample_count * dtype_size(b_->header.dtype) <= b_->storage.size());
    b_->header.sample_count = sample_count;
    detail::Block* b = b_;
    b_ = nullptr;
    // deleter が pool へ返す。以後 const でしか触れない。
    return BlockRef{std::shared_ptr<const detail::Block>(b, [](const detail::Block* p) {
        p->owner->release(const_cast<detail::Block*>(p));
    })};
}

} // namespace spear
