// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 音声フレームの誤り訂正。原本: crates/receiver-core/src/voice_frame.rs(commit dfc7440)。
//   デスクランブル後の 42 byte のうち byte 5–15 と 21–31 を交互に並べた 176 bit が内符号:
//   K=9 の畳み込み符号(0x1AF / 0x11D、voice_coordinates の位置でパンクチャ)、入力 94 bit = 78 bit のデータ +
//   8 bit の CRC(多項式 0x19B、位置 0–3 と 82–85)+ 8 bit の 0。
//   外側: byte 28–33 の CRC-16 を 34–35 に、生成多項式 [1,63,1,218,32,227,38] の短縮 RS(パリティ 6 byte)を 36–41 に。
//   モード: byte 32 = 0x80、byte 33 ≤ 1。逆変換 / 硬判定 Viterbi / 外符号の訂正 / 両者の合同訂正(上限つき)を順に試す。
//   出力: 158 bit(保護 78 bit + 非保護 80 bit = byte 0–4 と 16–20)を LSB first で 20 byte に。
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace spear::jr_train::protocol {

struct Speech {
    std::array<uint8_t, 20> payload{};
    uint16_t corrected_bits = 0;
};

std::optional<Speech> decode_voice(const std::array<uint8_t, 48>& envelope);

} // namespace spear::jr_train::protocol
