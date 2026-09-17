// AMBE 3600x2450 デコード(ambe/decoder.hpp の C++ 実装。pyambelib(mbelib-neo)と同じ PCM になることを golden test で保証)。
// フロー(std-t98-tools の audio service と同じ): 9 byte(3600) → fec_demod → 49 bit(2450, ThumbDV 並び) → [秘話なら PN を XOR] → 160 PCM @ 8 kHz。
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace spear::std_t98 {

// 3600 bps 9 byte ブロック → 49 bit(2450)。戻り値: 7 byte(先頭詰め、pyambelib の bits_to_bytes と同じ)。
std::array<uint8_t, 7> fec_demod_3600_to_2450(std::span<const uint8_t, 9> block, int* total_errors = nullptr);

class AmbeDecoder {
public:
    AmbeDecoder();
    ~AmbeDecoder();
    AmbeDecoder(const AmbeDecoder&) = delete;
    AmbeDecoder& operator=(const AmbeDecoder&) = delete;
    // 7 byte(2450) → 160 サンプル 8 kHz PCM。誤り情報なし(pyambelib / ThumbDV 相当)。チャネルごとに 1 インスタンス。
    void decode_2450(std::span<const uint8_t, 7> payload, std::span<int16_t, 160> pcm);
    // 9 byte(3600) → FEC(誤り数つき)→ [秘話: 49 bit に keystream を XOR] → PCM。誤りが多いフレームは JMBE 流にリピート/ミュートする。
    // keystream: mbelib d 順 49 bit(secret::keystream_d)。nullptr なら平文。戻り値は XOR 前の 2450 ペイロード(鍵探索の窓用)と誤り数。
    struct Decoded3600 { std::array<uint8_t, 7> raw2450{}; int total_errors = 0; };
    Decoded3600 decode_3600(std::span<const uint8_t, 9> block, std::span<int16_t, 160> pcm, const uint8_t* keystream = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace spear::std_t98
