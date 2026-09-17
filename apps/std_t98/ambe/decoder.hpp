// SPDX-License-Identifier: GPL-2.0-or-later
// AMBE 3600x2450(AMBE+2)デコーダ。mbelib(2010)/ mbelib-neo(2025, arancormonk)の AMBE 経路のみを C++ に書き直したもの
// (IMBE、3600x2400、SIMD 分岐、スレッドローカル状態、pffft は持たない)。アルゴリズムと定数は原本のまま。
// ビット並び(STD-T98 の 72 bit → 4×24 デインターリーブ、49 bit の ThumbDV 順)は tallcat4/pyambelib の解釈に従う。
#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <span>

namespace spear::std_t98::ambe {

using Bits49 = std::array<uint8_t, 49>;   // mbelib の ambe_d 順(b0 の MSB が [0])
using Frame4x24 = std::array<std::array<uint8_t, 24>, 4>;

// mbelib の ambe_d(49)⇔ ThumbDV/2450 ペイロードの並び: ThumbDV bit i = d[kThumbDv[i]]。
// 秘話の PN(core/crypto/secret_voice.py の THUMBDV_MAP)も同じ表なので、PN は d 順にそのまま XOR できる(secret/pn.hpp)。
inline constexpr std::array<int, 49> kThumbDv = {0, 18, 36, 1, 19, 37, 2, 20, 38, 3, 21, 39, 4, 22, 40, 5, 23, 41, 6, 24, 42, 7, 25, 43, 8, 26, 44,
                                                 9, 27, 45, 10, 28, 46, 11, 29, 47, 12, 30, 48, 13, 31, 14, 32, 15, 33, 16, 34, 17, 35};

// ---- 誤り訂正 ----
int golay2312(const uint8_t in[23], uint8_t out[23]);   // 戻り値: 訂正ビット数(12 データビット部分)

// ---- 3600 bps フレーム(9 byte)→ 49 bit ----
struct FecResult {
    Bits49 d{};            // mbelib 順のパラメータビット
    int c0_errors = 0;     // C0(Golay 24)訂正数
    int total_errors = 0;  // C0 + C1
};
void deinterleave_3600(std::span<const uint8_t, 9> block, Frame4x24& fr);   // STD-T98 の dibit 配置(pyambelib rW/rX/rY/rZ)
FecResult fec_decode(Frame4x24& fr);                                         // C0 Golay → PRNG 復調 → C1 Golay → 49 bit
std::array<uint8_t, 7> pack_thumbdv(const Bits49& d);                        // ThumbDV(2450)7 byte 並び
Bits49 unpack_thumbdv(std::span<const uint8_t, 7> payload);

// ---- MBE モデルパラメータ(原本 mbe_parms の AMBE で使う部分) ----
struct Params {
    float w0 = 0;
    int L = 0;
    std::array<int, 57> Vl{};
    std::array<float, 57> Ml{}, log2Ml{}, PHIl{}, PSIl{};
    float gamma = 0;
    int un = 0, repeat = 0, swn = 0;
    float localEnergy = 0;
    int amplitudeThreshold = 0;
    float errorRate = 0;
    int errorCountTotal = 0;
    int repeatCount = 0;
    std::array<float, 256> previousUw{};
    float noiseSeed = -1.f;
    std::array<float, 96> noiseOverlap{};
};

// ---- デコーダ(チャネルごとに 1 つ。前フレーム状態、乱数、FFT 作業領域を持つ) ----
class Decoder {
public:
    Decoder();
    // 49 bit → 160 サンプル(float、原本の float 経路: ±4447 でソフトクリップ)。
    // c0_valid=false のときは原本 Dataf 互換(total_errors > 3 でリピート)。
    void process(const Bits49& d, int c0_errors, int total_errors, bool c0_valid, std::span<float, 160> out);
    void reset();

private:
    enum class Kind { Voice, Erasure, Tone };
    Kind decode_params(const Bits49& d, int total_errors);
    void spectral_enhance(Params& p);
    void adaptive_smoothing(Params& cur, const Params& prev);
    void synthesize_speech(std::span<float, 160> out, Params& cur, Params& prev);
    void synthesize_unvoiced(std::span<float, 160> out, Params& cur, const Params& prev, const float noise[256]);
    void synthesize_tone(std::span<float, 160> out, const Bits49& d, Params& cur);
    void comfort_noise(std::span<float, 160> out);
    uint32_t java_next_bits(int bits);
    static void init_params(Params& p);
    static void erasure_params(Params& p, const Params& continuity);

    Params cur_, prev_, prev_enh_;
    uint64_t java_seed_ = 0;
    bool java_seeded_ = false;
    float pre_enh_rm0_ = 0;
    const Params* pre_enh_owner_ = nullptr;
    std::array<std::complex<float>, 256> fft_;
};

} // namespace spear::std_t98::ambe
