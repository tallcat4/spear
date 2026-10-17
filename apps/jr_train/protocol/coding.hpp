// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 符号化の共通部品。原本: crates/receiver-core/src/coding.rs(commit dfc7440)。
//   descramble: 48 byte の無線フレームからマーカー 3 つ(各 16 bit)を除いた 3 × 112 bit = 336 bit を取り出し、
//               PN9(pn[i] = pn[i−9] ^ pn[i−5]、初期値すべて 1)を XOR して 42 byte(LSB first)にする
//   crc16:      CRC-16(多項式 0x1021、初期値 0xFFFF、MSB first)
//   Galois:     GF(256)(多項式 0x11D)と RS の復号(Berlekamp–Massey + Chien 探索 + 連立方程式)。訂正後に全根を検証する
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace spear::jr_train::protocol {

std::array<uint8_t, 42> descramble(const std::array<uint8_t, 48>& envelope);
uint16_t crc16(std::span<const uint8_t> bytes);

class Galois {
public:
    static const Galois& get();
    uint8_t mul(uint8_t a, uint8_t b) const;
    uint8_t div(uint8_t a, uint8_t b) const;
    // roots 個の根(α^0..α^(roots−1))の RS 符号語を訂正する。戻り値: 訂正したシンボル数(訂正できなければ nullopt。word は変更されうる)
    std::optional<std::size_t> correct(std::span<uint8_t> word, std::size_t roots) const;
    std::array<uint8_t, 510> powers{};

private:
    Galois();
    std::array<std::size_t, 256> logs_{};
};

} // namespace spear::jr_train::protocol
