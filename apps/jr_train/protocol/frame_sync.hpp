// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// フレーム同期。原本: crates/receiver-core/src/frame.rs(commit dfc7440)。
//   1 フレーム = 384 bit = 192 シンボル = 40 ms = 48 byte。
//   bit 0 / 128 / 256 に 16 bit のマーカー 0x3D22 / 0xE867 / 0xE867。下位 14 bit の一致で数え、3 つ中 2 つ以上で一致。
//   未同期: 3 フレーム連続(1152 bit)一致する位置を探して捕捉。同期中: 3 フレーム続けて不一致で外れる。
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace spear::jr_train::protocol {

inline constexpr std::size_t kFrameBits = 384;
inline constexpr std::size_t kFrameBytes = 48;
inline constexpr std::array<uint16_t, 3> kMarkers = {0x3d22, 0xe867, 0xe867};

struct Frame {
    uint64_t bit_offset = 0;       // この同期器に入れた通算ビット位置(フレーム先頭)
    bool sync_matched = false;     // マーカー 3 つ中 2 つ以上一致
    uint32_t marker_errors = 0;    // 3 マーカー(16 bit 全体)のビット誤り数
    std::array<uint8_t, kFrameBytes> received{};   // MSB first、デスクランブル前
};

class Synchronizer {
public:
    // dibit(0..3)を 1 つ入れる。完成したフレームを out の末尾に追加する。
    void push(uint8_t dibit, std::vector<Frame>& out);
    // 1 byte に 4 dibit(上位から)詰めたもの(原本 packed)
    std::vector<Frame> packed(const uint8_t* bytes, std::size_t n);

    bool locked = false;
    uint64_t frames = 0;
    uint64_t synchronized_frames = 0;

private:
    std::pair<bool, uint32_t> markers(std::size_t offset) const;
    void drop_bits(std::size_t count);
    std::deque<uint8_t> bits_;
    uint64_t origin_ = 0;
    uint8_t failures_ = 0;
};

} // namespace spear::jr_train::protocol
