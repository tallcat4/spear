// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/receiver-core/src/frame.rs
#include "frame_sync.hpp"

#include <bit>
#include <stdexcept>

namespace spear::jr_train::protocol {

std::pair<bool, uint32_t> Synchronizer::markers(std::size_t offset) const {
    std::size_t matching = 0;
    uint32_t errors = 0;
    for (std::size_t m = 0; m < kMarkers.size(); ++m) {
        uint16_t value = 0;
        for (std::size_t i = 0; i < 16; ++i) value = static_cast<uint16_t>((value << 1) | bits_[offset + 128 * m + i]);
        if ((value & 0x3fff) == (kMarkers[m] & 0x3fff)) ++matching;
        errors += static_cast<uint32_t>(std::popcount(static_cast<uint16_t>(value ^ kMarkers[m])));
    }
    return {matching >= 2, errors};
}

void Synchronizer::drop_bits(std::size_t count) {
    bits_.erase(bits_.begin(), bits_.begin() + static_cast<std::ptrdiff_t>(count));
    origin_ += count;
}

void Synchronizer::push(uint8_t dibit, std::vector<Frame>& out) {
    if (dibit >= 4) throw std::invalid_argument("dibit must be 0..3");
    bits_.push_back(dibit >> 1);
    bits_.push_back(dibit & 1);
    if (!locked && bits_.size() >= 3 * kFrameBits) {
        std::size_t pos = 0;
        while (pos + 3 * kFrameBits <= bits_.size()) {
            if (markers(pos).first && markers(pos + kFrameBits).first && markers(pos + 2 * kFrameBits).first) {
                locked = true;
                failures_ = 0;
                break;
            }
            ++pos;
        }
        drop_bits(pos);
    }
    while (locked && bits_.size() >= kFrameBits) {
        const auto [sync_matched, marker_errors] = markers(0);
        Frame f;
        for (std::size_t i = 0; i < kFrameBits; ++i) f.received[i / 8] |= static_cast<uint8_t>(bits_[i] << (7 - i % 8));
        if (sync_matched) {
            failures_ = 0;
            ++synchronized_frames;
        } else {
            ++failures_;
        }
        ++frames;
        f.bit_offset = origin_;
        f.sync_matched = sync_matched;
        f.marker_errors = marker_errors;
        out.push_back(f);
        drop_bits(kFrameBits);
        if (failures_ >= 3) locked = false;
    }
}

std::vector<Frame> Synchronizer::packed(const uint8_t* bytes, std::size_t n) {
    std::vector<Frame> frames_out;
    for (std::size_t i = 0; i < n; ++i)
        for (int shift : {6, 4, 2, 0}) push(static_cast<uint8_t>((bytes[i] >> shift) & 3), frames_out);
    return frames_out;
}

} // namespace spear::jr_train::protocol
