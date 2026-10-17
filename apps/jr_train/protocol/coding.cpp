// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/receiver-core/src/coding.rs
#include "coding.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace spear::jr_train::protocol {

std::array<uint8_t, 42> descramble(const std::array<uint8_t, 48>& envelope) {
    std::array<uint8_t, 336> pn;
    pn.fill(1);
    std::array<uint8_t, 42> word{};
    for (std::size_t i = 0; i < 336; ++i) {
        if (i >= 9) pn[i] = pn[i - 9] ^ pn[i - 5];
        const std::size_t pos = 128 * (i / 112) + 16 + i % 112;
        word[i / 8] |= static_cast<uint8_t>((((envelope[pos / 8] >> (7 - pos % 8)) & 1) ^ pn[i]) << (i % 8));
    }
    return word;
}

uint16_t crc16(std::span<const uint8_t> bytes) {
    uint16_t crc = 0xffff;
    for (uint8_t b : bytes) {
        crc ^= static_cast<uint16_t>(b << 8);
        for (int k = 0; k < 8; ++k) crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

Galois::Galois() {
    std::size_t v = 1;
    for (std::size_t i = 0; i < 255; ++i) {
        powers[i] = static_cast<uint8_t>(v);
        powers[i + 255] = static_cast<uint8_t>(v);
        logs_[v] = i;
        v <<= 1;
        if (v & 256) v ^= 0x11d;
    }
}

const Galois& Galois::get() {
    static const Galois gf;
    return gf;
}

uint8_t Galois::mul(uint8_t a, uint8_t b) const {
    if (a == 0 || b == 0) return 0;
    return powers[logs_[a] + logs_[b]];
}

uint8_t Galois::div(uint8_t a, uint8_t b) const {
    if (b == 0) throw std::logic_error("GF division by zero");
    if (a == 0) return 0;
    return powers[(logs_[a] + 255 - logs_[b]) % 255];
}

namespace {
std::vector<uint8_t> syndromes(const Galois& gf, std::span<const uint8_t> word, std::size_t roots) {
    std::vector<uint8_t> s(roots);
    for (std::size_t r = 0; r < roots; ++r) {
        uint8_t v = 0;
        for (uint8_t b : word) v = static_cast<uint8_t>(gf.mul(v, gf.powers[r]) ^ b);
        s[r] = v;
    }
    return s;
}
} // namespace

std::optional<std::size_t> Galois::correct(std::span<uint8_t> word, std::size_t roots) const {
    const auto syn = syndromes(*this, word, roots);
    if (std::all_of(syn.begin(), syn.end(), [](uint8_t s) { return s == 0; })) return 0;
    std::vector<uint8_t> loc(roots + 1, 0), prev(roots + 1, 0);
    loc[0] = 1;
    prev[0] = 1;
    std::size_t degree = 0, gap = 1;
    uint8_t last = 1;
    for (std::size_t step = 0; step < roots; ++step) {
        uint8_t d = syn[step];
        for (std::size_t i = 1; i <= degree; ++i) d ^= mul(loc[i], syn[step - i]);
        if (d == 0) {
            ++gap;
            continue;
        }
        const auto saved = loc;
        const uint8_t scale = div(d, last);
        for (std::size_t i = 0; i + gap < roots + 1; ++i) loc[i + gap] ^= mul(scale, prev[i]);
        if (2 * degree <= step) {
            degree = step + 1 - degree;
            prev = saved;
            last = d;
            gap = 1;
        } else {
            ++gap;
        }
    }
    if (degree == 0 || degree > roots / 2) return std::nullopt;
    const std::size_t n = word.size();
    std::vector<std::size_t> positions;
    for (std::size_t p = 0; p < n; ++p) {
        const uint8_t inverse = powers[(p + 255 - (n - 1)) % 255];
        uint8_t v = 0;
        for (std::size_t i = degree + 1; i-- > 0;) v = static_cast<uint8_t>(mul(v, inverse) ^ loc[i]);
        if (v == 0) positions.push_back(p);
    }
    if (positions.size() != degree) return std::nullopt;
    std::vector<std::vector<uint8_t>> mat(degree, std::vector<uint8_t>(degree + 1, 0));
    for (std::size_t row = 0; row < degree; ++row) {
        for (std::size_t col = 0; col < degree; ++col) mat[row][col] = powers[(row * (n - 1 - positions[col])) % 255];
        mat[row][degree] = syn[row];
    }
    for (std::size_t col = 0; col < degree; ++col) {
        std::size_t pivot = col;
        while (pivot < degree && mat[pivot][col] == 0) ++pivot;
        if (pivot == degree) return std::nullopt;
        std::swap(mat[pivot], mat[col]);
        const uint8_t scale = mat[col][col];
        for (std::size_t i = col; i <= degree; ++i) mat[col][i] = div(mat[col][i], scale);
        for (std::size_t row = 0; row < degree; ++row) {
            if (row == col) continue;
            const uint8_t factor = mat[row][col];
            for (std::size_t i = col; i <= degree; ++i) mat[row][i] ^= mul(factor, mat[col][i]);
        }
    }
    for (std::size_t i = 0; i < degree; ++i) word[positions[i]] ^= mat[i][degree];
    const auto check = syndromes(*this, word, roots);
    if (!std::all_of(check.begin(), check.end(), [](uint8_t s) { return s == 0; })) return std::nullopt;
    return degree;
}

} // namespace spear::jr_train::protocol
