// SPDX-License-Identifier: GPL-2.0-or-later
// mbelib / mbelib-neo の AMBE 3600x2450 経路の C++ 移植。原本: ecc.c, ambe_common.c, ambe3600x2450.c, mbelib.c,
// mbe_adaptive.c, mbe_unvoiced_fft.c(JMBE Algorithms #111-#140 の番号は原本のコメントに対応)。
#include "decoder.hpp"
#include "tables.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <numbers>

namespace spear::std_t98::ambe {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = 2.f * kPi;
constexpr int kN = 160;                 // 1 フレームの出力サンプル数
constexpr int kFft = 256;
constexpr float kSoftClip = (32767.f * 0.95f) / 7.f;   // JMBE の float 経路ソフトクリップ
constexpr float kUnvoicedScale = 146.17696f;
constexpr float k256Over2Pi = 256.f / (2.f * 3.14159265358979323846f);
constexpr float kWhiteNoiseScalar = 2.f * kPi / 53125.f;
constexpr int kMaxFrameRepeats = 4;
constexpr float kDefaultLocalEnergy = 75000.f, kMinLocalEnergy = 10000.f;
constexpr int kDefaultAmplitudeThreshold = 20480;

// ---- Golay(23,12): 原本 golayGenerator と、原本 golayMatrix(2048)と同値の syndrome → データ誤りパターン表 ----
constexpr int kGolayGen[12] = {0x63a, 0x31d, 0x7b4, 0x3da, 0x1ed, 0x6cc, 0x366, 0x1b3, 0x6e3, 0x54b, 0x49f, 0x475};

int golay_ecc_of(uint32_t codeword23) {
    int ecc = 0;
    uint32_t mask = 0x400000u;
    for (int i = 0; i < 12; ++i) { if (codeword23 & mask) ecc ^= kGolayGen[i]; mask >>= 1; }
    return ecc;
}

// 重み ≤ 3 の全誤りパターンから syndrome → データ部誤りビット(12 bit)を構成する(= 原本の golayMatrix)
const std::array<int, 2048>& golay_matrix() {
    static const std::array<int, 2048> m = [] {
        std::array<int, 2048> t{};
        std::array<bool, 2048> have{};
        auto add = [&](uint32_t e) {
            const int syn = golay_ecc_of(e) ^ static_cast<int>(e & 0x7ffu);
            if (!have[static_cast<std::size_t>(syn)]) { have[static_cast<std::size_t>(syn)] = true; t[static_cast<std::size_t>(syn)] = static_cast<int>(e >> 11); }
        };
        add(0);
        for (int a = 0; a < 23; ++a) {
            add(1u << a);
            for (int b = a + 1; b < 23; ++b) {
                add((1u << a) | (1u << b));
                for (int c = b + 1; c < 23; ++c) add((1u << a) | (1u << b) | (1u << c));
            }
        }
        return t;
    }();
    return m;
}

// ---- STD-T98 の dibit → 4×24 配置(pyambelib libambe_wrapper.c) ----
constexpr int kW[36] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2};
constexpr int kX[36] = {23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10, 11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4};
constexpr int kY[36] = {0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3};
constexpr int kZ[36] = {5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12, 22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6, 16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0};
// mbelib の ambe_d(49)⇔ ThumbDV/2450 ペイロードの並び
constexpr int kThumbDv[49] = {0, 18, 36, 1, 19, 37, 2, 20, 38, 3, 21, 39, 4, 22, 40, 5, 23, 41, 6, 24, 42, 7, 25, 43, 8, 26, 44,
                              9, 27, 45, 10, 28, 46, 11, 29, 47, 12, 30, 48, 13, 31, 14, 32, 15, 33, 16, 34, 17, 35};

inline float ws_unvoiced(int n) { return (n < -105 || n > 105) ? 0.f : tables::kWsUnvoiced[n + 105]; }

inline int bits(const Bits49& d, int from, int to) {   // d[from..to) を MSB first で整数に
    int v = 0;
    for (int i = from; i < to; ++i) v = (v << 1) | d[static_cast<std::size_t>(i)];
    return v;
}

// 256 点複素 FFT(radix-2、in place)。inverse は 1/N を掛けない(呼び側で正規化)。
void fft256(std::array<std::complex<float>, 256>& x, bool inverse) {
    for (unsigned i = 1, j = 0; i < 256; ++i) {
        unsigned bit = 128;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (unsigned len = 2; len <= 256; len <<= 1) {
        const float ang = (inverse ? 2.f : -2.f) * kPi / static_cast<float>(len);
        for (unsigned i = 0; i < 256; i += len) {
            for (unsigned k = 0; k < len / 2; ++k) {
                const std::complex<float> w(std::cos(ang * static_cast<float>(k)), std::sin(ang * static_cast<float>(k)));
                const auto u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
            }
        }
    }
}

} // namespace

// ============================================================ ECC / フレーム ============================================================

int golay2312(const uint8_t in[23], uint8_t out[23]) {
    uint32_t block = 0;
    for (int i = 22; i >= 0; --i) block = (block << 1) | (in[i] & 1u);
    const int syndrome = golay_ecc_of(block) ^ static_cast<int>(block & 0x7ffu);
    uint32_t data = (block >> 11) ^ static_cast<uint32_t>(golay_matrix()[static_cast<std::size_t>(syndrome)]);
    for (int i = 22; i >= 11; --i) { out[i] = static_cast<uint8_t>((data & 2048u) >> 11); data <<= 1; }
    for (int i = 10; i >= 0; --i) out[i] = in[i];
    int errs = 0;
    for (int i = 22; i >= 11; --i) if (out[i] != in[i]) ++errs;
    return errs;
}

void deinterleave_3600(std::span<const uint8_t, 9> block, Frame4x24& fr) {
    uint8_t b[72];
    int k = 0;
    for (uint8_t byte : block) for (int j = 7; j >= 0; --j) b[k++] = static_cast<uint8_t>((byte >> j) & 1);
    for (auto& row : fr) row.fill(0);
    for (int s = 0; s < 36; ++s) {
        fr[static_cast<std::size_t>(kW[s])][static_cast<std::size_t>(kX[s])] = b[s * 2];
        fr[static_cast<std::size_t>(kY[s])][static_cast<std::size_t>(kZ[s])] = b[s * 2 + 1];
    }
}

FecResult fec_decode(Frame4x24& fr) {
    FecResult r;
    // C0: Golay(23,12) on fr[0][1..23]; 無誤りなら Golay24 の偶数パリティで fr[0][0] を確認(JMBE)
    {
        uint8_t in[23], out[23];
        for (int j = 0; j < 23; ++j) in[j] = fr[0][static_cast<std::size_t>(j + 1)];
        r.c0_errors = golay2312(in, out);
        for (int j = 0; j < 23; ++j) fr[0][static_cast<std::size_t>(j + 1)] = out[j];
        if (r.c0_errors == 0) {
            int ones = 0;
            for (int j = 0; j < 24; ++j) ones += fr[0][static_cast<std::size_t>(j)] & 1;
            if (ones & 1) { fr[0][0] ^= 1; r.c0_errors = 1; }
        }
    }
    // PRNG 復調: C0 の 12 bit を種にした LCG で C1 をデスクランブル
    {
        uint16_t pr[24];
        uint16_t seed = 0;
        for (int i = 23; i >= 12; --i) seed = static_cast<uint16_t>((seed << 1) | fr[0][static_cast<std::size_t>(i)]);
        pr[0] = static_cast<uint16_t>(16 * seed);
        for (int i = 1; i < 24; ++i) pr[i] = static_cast<uint16_t>((173u * pr[i - 1] + 13849u) % 65536u);
        for (int i = 1; i < 24; ++i) pr[i] = static_cast<uint16_t>(pr[i] / 32768u);
        int k = 1;
        for (int j = 22; j >= 0; --j) fr[1][static_cast<std::size_t>(j)] ^= static_cast<uint8_t>(pr[k++]);
    }
    // 49 bit: C0[23..12] | golay(C1)[22..11] | C2[10..0] | C3[13..0]
    {
        std::size_t o = 0;
        for (int j = 23; j > 11; --j) r.d[o++] = fr[0][static_cast<std::size_t>(j)];
        uint8_t gin[23], gout[23];
        for (int j = 0; j < 23; ++j) gin[j] = fr[1][static_cast<std::size_t>(j)];
        const int e1 = golay2312(gin, gout);
        for (int j = 22; j > 10; --j) r.d[o++] = gout[j];
        for (int j = 10; j >= 0; --j) r.d[o++] = fr[2][static_cast<std::size_t>(j)];
        for (int j = 13; j >= 0; --j) r.d[o++] = fr[3][static_cast<std::size_t>(j)];
        r.total_errors = r.c0_errors + e1;
    }
    return r;
}

std::array<uint8_t, 7> pack_thumbdv(const Bits49& d) {
    std::array<uint8_t, 7> out{};
    for (int i = 0; i < 49; ++i) out[static_cast<std::size_t>(i / 8)] |= static_cast<uint8_t>((d[static_cast<std::size_t>(kThumbDv[i])] & 1) << (7 - i % 8));
    return out;
}

Bits49 unpack_thumbdv(std::span<const uint8_t, 7> payload) {
    Bits49 d{};
    for (int i = 0; i < 49; ++i) d[static_cast<std::size_t>(kThumbDv[i])] = static_cast<uint8_t>((payload[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1);
    return d;
}

// ============================================================ デコーダ ============================================================

Decoder::Decoder() { reset(); }

void Decoder::init_params(Params& p) {   // 原本 mbe_initAmbeParms_common(JMBE W124 既定)
    p = Params{};
    p.w0 = static_cast<float>((std::numbers::pi / 32.0) * (2.0 * std::numbers::pi));
    p.L = 15;
    p.Ml.fill(1.f);
    p.localEnergy = kDefaultLocalEnergy;
    p.amplitudeThreshold = kDefaultAmplitudeThreshold;
    p.noiseSeed = -1.f;
}

void Decoder::erasure_params(Params& p, const Params& c) {   // 原本 mbe_setAmbeErasureParms_common(JMBE W120..W123)
    p.swn = 0; p.un = 0; p.w0 = 0.f; p.L = 9; p.gamma = 0.f;
    for (int l = 0; l <= 56; ++l) {
        p.Ml[static_cast<std::size_t>(l)] = 1.f; p.Vl[static_cast<std::size_t>(l)] = 0; p.log2Ml[static_cast<std::size_t>(l)] = 0.f;
        p.PHIl[static_cast<std::size_t>(l)] = c.PHIl[static_cast<std::size_t>(l)]; p.PSIl[static_cast<std::size_t>(l)] = c.PSIl[static_cast<std::size_t>(l)];
    }
    p.localEnergy = kDefaultLocalEnergy;
    p.amplitudeThreshold = kDefaultAmplitudeThreshold;
    p.noiseSeed = c.noiseSeed;
    p.noiseOverlap = c.noiseOverlap;
    p.previousUw = c.previousUw;
}

void Decoder::reset() {
    init_params(prev_);
    cur_ = prev_;
    prev_enh_ = prev_;
    java_seeded_ = false;
    pre_enh_owner_ = nullptr;
}

uint32_t Decoder::java_next_bits(int nbits) {   // java.util.Random 互換 48 bit LCG(コンフォートノイズ)
    constexpr uint64_t kMult = 0x5DEECE66DULL, kAdd = 0xBULL, kMask = (1ULL << 48) - 1ULL, kInit = 0x12345678ULL;
    if (!java_seeded_) { java_seed_ = (kInit ^ kMult) & kMask; java_seeded_ = true; }
    java_seed_ = (java_seed_ * kMult + kAdd) & kMask;
    return static_cast<uint32_t>(java_seed_ >> (48 - nbits));
}

void Decoder::comfort_noise(std::span<float, 160> out) {   // JMBE muted-noise: 一様白色雑音 [-1,1] × 0.003
    const float gain = (0.003f * 32767.f) / 7.f;
    for (auto& v : out) v = ((static_cast<float>(java_next_bits(24)) / 16777216.f) * 2.f - 1.f) * gain;
}

// ---- 49 bit → モデルパラメータ(原本 mbe_decodeAmbe2450ParmsInternal) ----
Decoder::Kind Decoder::decode_params(const Bits49& d, int total_errors) {
    Params& cur = cur_;
    Params& prev = prev_;
    const int u0 = bits(d, 0, 12), u1 = bits(d, 12, 24), u3 = bits(d, 35, 49);
    const bool tone_verified = ((u0 >> 6) & 0x3f) == 63 && ((u3 & 0xf) == 0 || ((u1 >> 8) & 0xf) == (u1 & 0xf));

    cur.repeat = prev.repeat;
    const int b0 = (d[0] << 6) | (d[1] << 5) | (d[2] << 4) | (d[3] << 3) | (d[37] << 2) | (d[38] << 1) | d[39];
    if (tone_verified && total_errors < 6) return Kind::Tone;
    if (b0 >= 120 && b0 <= 123) return Kind::Erasure;
    bool silence = false;
    float f0 = 0.f;
    int L = 0;
    if (b0 == 124 || b0 == 125) {
        silence = true;
        f0 = kPi / 32.f;
        cur.w0 = f0 * static_cast<float>(2.0 * std::numbers::pi);
        L = (b0 == 124) ? 15 : 14;
        cur.L = L;
        for (int l = 1; l <= L; ++l) cur.Vl[static_cast<std::size_t>(l)] = 0;
    }
    if (b0 == 126 || b0 == 127) return Kind::Erasure;
    if (!silence) {
        f0 = tables::kW0[b0];
        cur.w0 = f0 * static_cast<float>(2) * static_cast<float>(std::numbers::pi);
        L = static_cast<int>(tables::kL[b0]);
        cur.L = L;
    }
    const float unvc = 0.2046f / std::sqrt(cur.w0);

    // V/UV
    const int b1 = (d[4] << 4) | (d[5] << 3) | (d[6] << 2) | (d[7] << 1) | d[35];
    for (int l = 1; l <= L; ++l) {
        const int jl = static_cast<int>(static_cast<float>(l) * 16.f * f0);
        if (!silence) cur.Vl[static_cast<std::size_t>(l)] = tables::kVuv[b1][jl];
    }
    // ゲイン
    const int b2 = (d[8] << 4) | (d[9] << 3) | (d[10] << 2) | (d[11] << 1) | d[36];
    cur.gamma = tables::kDg[b2] + 0.5f * prev.gamma;
    // PRBA
    float Gm[9] = {};
    const int b3 = (d[12] << 8) | (d[13] << 7) | (d[14] << 6) | (d[15] << 5) | (d[16] << 4) | (d[17] << 3) | (d[18] << 2) | (d[19] << 1) | d[40];
    Gm[2] = tables::kPrba24[b3][0]; Gm[3] = tables::kPrba24[b3][1]; Gm[4] = tables::kPrba24[b3][2];
    const int b4 = (d[20] << 6) | (d[21] << 5) | (d[22] << 4) | (d[23] << 3) | (d[41] << 2) | (d[42] << 1) | d[43];
    Gm[5] = tables::kPrba58[b4][0]; Gm[6] = tables::kPrba58[b4][1]; Gm[7] = tables::kPrba58[b4][2]; Gm[8] = tables::kPrba58[b4][3];
    float Ri[9];
    for (int i = 1; i <= 8; ++i) {
        float sum = 0;
        for (int m = 1; m <= 8; ++m) {
            const float am = (m == 1) ? 1.f : 2.f;
            sum += am * Gm[m] * std::cos((kPi * static_cast<float>(m - 1) * (static_cast<float>(i) - 0.5f)) / 8.f);
        }
        Ri[i] = sum;
    }
    float Cik[5][18] = {};
    const float rconst = 1.f / (2.f * static_cast<float>(std::numbers::sqrt2));
    Cik[1][1] = 0.5f * (Ri[1] + Ri[2]); Cik[1][2] = rconst * (Ri[1] - Ri[2]);
    Cik[2][1] = 0.5f * (Ri[3] + Ri[4]); Cik[2][2] = rconst * (Ri[3] - Ri[4]);
    Cik[3][1] = 0.5f * (Ri[5] + Ri[6]); Cik[3][2] = rconst * (Ri[5] - Ri[6]);
    Cik[4][1] = 0.5f * (Ri[7] + Ri[8]); Cik[4][2] = rconst * (Ri[7] - Ri[8]);
    // HOC
    const int b5 = (d[24] << 4) | (d[25] << 3) | (d[26] << 2) | (d[27] << 1) | d[44];
    const int b6 = (d[28] << 3) | (d[29] << 2) | (d[30] << 1) | d[45];
    const int b7 = (d[31] << 3) | (d[32] << 2) | (d[33] << 1) | d[46];
    const int b8 = (d[34] << 2) | (d[47] << 1) | d[48];
    int Ji[5];
    for (int i = 1; i <= 4; ++i) Ji[i] = tables::kLmprbl[L][i - 1];
    for (int k = 3; k <= Ji[1]; ++k) Cik[1][k] = k > 6 ? 0.f : tables::kHocB5[b5][k - 3];
    for (int k = 3; k <= Ji[2]; ++k) Cik[2][k] = k > 6 ? 0.f : tables::kHocB6[b6][k - 3];
    for (int k = 3; k <= Ji[3]; ++k) Cik[3][k] = k > 6 ? 0.f : tables::kHocB7[b7][k - 3];
    for (int k = 3; k <= Ji[4]; ++k) Cik[4][k] = k > 6 ? 0.f : tables::kHocB8[b8][k - 3];
    // 各ブロックの逆 DCT → Tl
    float Tl[57] = {};
    {
        int l = 1;
        for (int i = 1; i <= 4; ++i) {
            const int ji = Ji[i];
            for (int j = 1; j <= ji; ++j) {
                float sum = 0;
                for (int k = 1; k <= ji; ++k) {
                    const float ak = (k == 1) ? 1.f : 2.f;
                    sum += ak * Cik[i][k] * std::cos((kPi * static_cast<float>(k - 1) * (static_cast<float>(j) - 0.5f)) / static_cast<float>(ji));
                }
                Tl[l++] = sum;
            }
        }
    }
    // 前フレームの log2Ml を今の L に合わせて予測(eq. 40-43)
    if (cur.L > prev.L)
        for (int l = prev.L + 1; l <= cur.L; ++l) { prev.Ml[static_cast<std::size_t>(l)] = prev.Ml[static_cast<std::size_t>(prev.L)]; prev.log2Ml[static_cast<std::size_t>(l)] = prev.log2Ml[static_cast<std::size_t>(prev.L)]; }
    prev.log2Ml[0] = prev.log2Ml[1];
    prev.Ml[0] = prev.Ml[1];
    int intkl[57];
    float deltal[57];
    float Sum43 = 0;
    for (int l = 1; l <= cur.L; ++l) {
        const float flokl = (static_cast<float>(prev.L) / static_cast<float>(cur.L)) * static_cast<float>(l);
        intkl[l] = static_cast<int>(flokl);
        deltal[l] = flokl - static_cast<float>(intkl[l]);
        Sum43 += ((1.f - deltal[l]) * prev.log2Ml[static_cast<std::size_t>(intkl[l])]) + (deltal[l] * prev.log2Ml[static_cast<std::size_t>(intkl[l] + 1)]);
    }
    Sum43 = (0.65f / static_cast<float>(cur.L)) * Sum43;
    float Sum42 = 0;
    for (int l = 1; l <= cur.L; ++l) Sum42 += Tl[l];
    Sum42 /= static_cast<float>(cur.L);
    const float BigGamma = cur.gamma - (0.5f * std::log2(static_cast<float>(cur.L))) - Sum42;
    for (int l = 1; l <= cur.L; ++l) {
        const float c1 = 0.65f * (1.f - deltal[l]) * prev.log2Ml[static_cast<std::size_t>(intkl[l])];
        const float c2 = 0.65f * deltal[l] * prev.log2Ml[static_cast<std::size_t>(intkl[l] + 1)];
        cur.log2Ml[static_cast<std::size_t>(l)] = Tl[l] + c1 + c2 - Sum43 + BigGamma;
        cur.Ml[static_cast<std::size_t>(l)] = (cur.Vl[static_cast<std::size_t>(l)] == 1 ? 1.f : unvc) * std::exp2(cur.log2Ml[static_cast<std::size_t>(l)]);
    }
    return Kind::Voice;
}

// ---- スペクトル振幅強調(原本 mbe_spectralAmpEnhance) ----
void Decoder::spectral_enhance(Params& p) {
    const int L = p.L;
    float cos_tab[57];
    {
        const float s_step = std::sin(p.w0), c_step = std::cos(p.w0);
        float c = 1.f, s = 0.f;
        for (int l = 1; l <= L; ++l) { const float cn = c * c_step - s * s_step, sn = s * c_step + c * s_step; c = cn; s = sn; cos_tab[l] = c; }
    }
    float Rm0 = 0, Rm1 = 0;
    for (int l = 1; l <= L; ++l) { const float m2 = p.Ml[static_cast<std::size_t>(l)] * p.Ml[static_cast<std::size_t>(l)]; Rm0 += m2; Rm1 += m2 * cos_tab[l]; }
    pre_enh_rm0_ = Rm0; pre_enh_owner_ = &p;   // Algorithm #111 は強調前の RM0 を使う
    const float R2m0 = Rm0 * Rm0, R2m1 = Rm1 * Rm1;
    for (int l = 1; l <= L; ++l) {
        float& M = p.Ml[static_cast<std::size_t>(l)];
        if (M == 0.f) continue;
        const float Wl = std::sqrt(M) * std::sqrt(std::sqrt((0.96f * kPi * ((R2m0 + R2m1) - (2.f * Rm0 * Rm1 * cos_tab[l]))) / (p.w0 * Rm0 * (R2m0 - R2m1))));
        if (8 * l <= L) { }
        else if (Wl > 1.2f) M = 1.2f * M;
        else if (Wl < 0.5f) M = 0.5f * M;
        else M = Wl * M;
    }
    float sum = 0;
    for (int l = 1; l <= L; ++l) { const float M = std::fabs(p.Ml[static_cast<std::size_t>(l)]); sum += M * M; }
    const float gamma = (sum == 0.f) ? 1.f : std::sqrt(Rm0 / sum);
    for (int l = 1; l <= L; ++l) p.Ml[static_cast<std::size_t>(l)] *= gamma;
}

// ---- 適応平滑化(原本 mbe_applyAdaptiveSmoothing、Algorithms #111-116) ----
void Decoder::adaptive_smoothing(Params& cur, const Params& prev) {
    const int L = cur.L;
    const float errorRate = cur.errorRate;
    const int errorTotal = cur.errorCountTotal;
    float RM0 = 0;
    if (pre_enh_owner_ == &cur) { RM0 = pre_enh_rm0_; pre_enh_owner_ = nullptr; }
    else for (int l = 1; l <= L; ++l) RM0 += cur.Ml[static_cast<std::size_t>(l)] * cur.Ml[static_cast<std::size_t>(l)];
    float prevEnergy = prev.localEnergy;
    if (prevEnergy < kMinLocalEnergy) prevEnergy = kDefaultLocalEnergy;
    cur.localEnergy = 0.95f * prevEnergy + 0.05f * RM0;
    if (cur.localEnergy < kMinLocalEnergy) cur.localEnergy = kMinLocalEnergy;
    float VM;
    if (errorRate <= 0.005f && errorTotal <= 4) VM = FLT_MAX;
    else {
        const float x8 = std::sqrt(std::sqrt(std::sqrt(cur.localEnergy)));
        const float energy = x8 * x8 * x8;
        VM = (errorRate <= 0.0125f) ? (45.255f * energy) / std::exp(277.26f * errorRate) : 1.414f * energy;   // AMBE: errorCount4 = 0
    }
    for (int l = 1; l <= L; ++l) if (cur.Ml[static_cast<std::size_t>(l)] > VM) cur.Vl[static_cast<std::size_t>(l)] = 1;
    float Am = 0;
    for (int l = 1; l <= L; ++l) Am += cur.Ml[static_cast<std::size_t>(l)];
    int prevThreshold = prev.amplitudeThreshold;
    if (prevThreshold <= 0) prevThreshold = kDefaultAmplitudeThreshold;
    const int Tm = (errorRate <= 0.005f && errorTotal <= 6) ? kDefaultAmplitudeThreshold : 6000 - 300 * errorTotal + prevThreshold;
    cur.amplitudeThreshold = Tm;
    if (Am > static_cast<float>(Tm) && Am > 0.f) {
        const float scale = static_cast<float>(Tm) / Am;
        for (int l = 1; l <= L; ++l) cur.Ml[static_cast<std::size_t>(l)] *= scale;
    }
}

// ---- 無声成分: 白色雑音の 256 点 FFT を帯域ごとにスケール → IFFT → WOLA(Algorithms #117-126) ----
void Decoder::synthesize_unvoiced(std::span<float, 160> out, Params& cur, const Params& prev, const float noise[256]) {
    const int L = cur.L;
    for (int i = 0; i < kFft; ++i) fft_[static_cast<std::size_t>(i)] = {noise[i] * ws_unvoiced(i - 128), 0.f};
    fft256(fft_, false);
    float scalor[kFft / 2 + 1] = {};
    const float mult = k256Over2Pi * cur.w0;
    for (int l = 1; l <= L; ++l) {
        if (cur.Vl[static_cast<std::size_t>(l)] != 0) continue;
        int a = static_cast<int>(std::ceil((static_cast<float>(l) - 0.5f) * mult));
        int b = static_cast<int>(std::ceil((static_cast<float>(l) + 0.5f) * mult));
        a = std::max(a, 0); b = std::min(b, kFft / 2);
        if (b <= a) continue;
        float num = 0;
        for (int bin = a; bin < b; ++bin) num += std::norm(fft_[static_cast<std::size_t>(bin)]);
        if (num > 1e-10f) {
            const float s = kUnvoicedScale * cur.Ml[static_cast<std::size_t>(l)] / std::sqrt(num / static_cast<float>(b - a));
            for (int bin = a; bin < b; ++bin) scalor[bin] = s;
        }
    }
    // 実信号なので bin k と 256-k を同じ係数で(有声帯域は 0)
    for (int bin = 0; bin <= kFft / 2; ++bin) {
        fft_[static_cast<std::size_t>(bin)] *= scalor[bin];
        if (bin > 0 && bin < kFft / 2) fft_[static_cast<std::size_t>(kFft - bin)] *= scalor[bin];
    }
    fft256(fft_, true);
    float Uw[kFft];
    for (int i = 0; i < kFft; ++i) Uw[i] = fft_[static_cast<std::size_t>(i)].real() / static_cast<float>(kFft);
    for (int n = 0; n < kN; ++n) {
        const float wp = ws_unvoiced(n), wc = ws_unvoiced(n - kN);
        const float ps = (n + 128 < kFft) ? prev.previousUw[static_cast<std::size_t>(n + 128)] : 0.f;
        const float cs = (n - 32 >= 0) ? Uw[n - 32] : 0.f;
        const float denom = wp * wp + wc * wc;
        if (denom > 1e-10f) out[static_cast<std::size_t>(n)] += ((wp * ps) + (wc * cs)) / denom;
    }
    std::copy(Uw, Uw + kFft, cur.previousUw.begin());
}

// ---- 音声合成(原本 mbe_synthesizeSpeechf) ----
void Decoder::synthesize_speech(std::span<float, 160> out, Params& cur, Params& prev) {
    adaptive_smoothing(cur, prev);
    if (cur.repeatCount >= kMaxFrameRepeats) { comfort_noise(out); return; }   // AMBE 経路は最大リピートのみでミュート

    // Algorithm #117: 雑音 256 点(LCG 171/11213/53125、96 サンプル重なり)。位相ジッタと無声合成で共用
    float noise[kFft];
    if (cur.noiseSeed < 0.f) { std::fill(noise, noise + kFft, 0.f); cur.noiseOverlap.fill(0.f); cur.noiseSeed = 3147.f; }
    else {
        std::copy(cur.noiseOverlap.begin(), cur.noiseOverlap.end(), noise);
        unsigned state = static_cast<unsigned>(cur.noiseSeed) % 53125u;
        for (int i = 96; i < kFft; ++i) { noise[i] = static_cast<float>(state); state = (171u * state + 11213u) % 53125u; }
        cur.noiseSeed = static_cast<float>(state);
        std::copy(noise + kFft - 96, noise + kFft, cur.noiseOverlap.begin());
    }
    int numUv = 0;
    for (int l = 0; l <= cur.L; ++l) if (cur.Vl[static_cast<std::size_t>(l)] == 0) ++numUv;
    const float cw0 = cur.w0, pw0 = prev.w0;
    std::fill(out.begin(), out.end(), 0.f);
    int maxl;
    if (cur.L > prev.L) { maxl = cur.L; for (int l = prev.L + 1; l <= maxl; ++l) { prev.Ml[static_cast<std::size_t>(l)] = 0.f; prev.Vl[static_cast<std::size_t>(l)] = 1; } }
    else { maxl = prev.L; for (int l = cur.L + 1; l <= maxl; ++l) { cur.Ml[static_cast<std::size_t>(l)] = 0.f; cur.Vl[static_cast<std::size_t>(l)] = 1; } }
    // 位相(eq. 139, 140)
    for (int l = 1; l <= 56; ++l) {
        const auto li = static_cast<std::size_t>(l);
        float pw = std::fmod(prev.PSIl[li], kTwoPi);
        if (pw < 0.f) pw += kTwoPi;
        prev.PSIl[li] = pw;
        cur.PSIl[li] = pw + ((pw0 + cw0) * (static_cast<float>(l * kN) / 2.f));
        if (l <= cur.L / 4) cur.PHIl[li] = cur.PSIl[li];
        else {
            const float pl = (kWhiteNoiseScalar * noise[l]) - kPi;
            cur.PHIl[li] = cur.PSIl[li] + ((static_cast<float>(numUv) * pl) / static_cast<float>(cur.L));
        }
    }
    // 有声成分
    for (int l = 1; l <= maxl; ++l) {
        const auto li = static_cast<std::size_t>(l);
        const float cw0l = cw0 * static_cast<float>(l), pw0l = pw0 * static_cast<float>(l);
        const bool cur_v = cur.Vl[li] == 1, prev_v = prev.Vl[li] == 1;
        if (!cur_v && !prev_v) continue;
        const bool interp = (l < 8) && cur_v && prev_v && (std::fabs(cw0 - pw0) < (0.1f * cw0));
        if (interp) {   // Algorithms #134-138: 位相・振幅補間
            const float deltaphil = cur.PHIl[li] - prev.PHIl[li] - (((pw0 + cw0) * static_cast<float>(l * kN)) / 2.f);
            const float deltawl = (1.f / static_cast<float>(kN)) * (deltaphil - (2.f * kPi * std::floor((deltaphil + kPi) / (2.f * kPi))));
            for (int n = 0; n < kN; ++n) {
                const float theta = prev.PHIl[li] + ((pw0l + deltawl) * static_cast<float>(n)) + (((cw0 - pw0) * static_cast<float>(l * n * n)) / static_cast<float>(2 * kN));
                const float aln = prev.Ml[li] + ((static_cast<float>(n) / static_cast<float>(kN)) * (cur.Ml[li] - prev.Ml[li]));
                out[static_cast<std::size_t>(n)] += 2.f * aln * std::cos(theta);
            }
        } else {        // 窓付き発振器(前フレーム成分のフェードアウト + 現フレーム成分のフェードイン)
            auto add = [&](float amp, float phase0, float step, const float* W) {
                const float sd = std::sin(step), cd = std::cos(step);
                float s = std::sin(phase0), c = std::cos(phase0);
                for (int n = 0; n < kN; ++n) {
                    out[static_cast<std::size_t>(n)] += 2.f * W[n] * amp * c;
                    const float cn = c * cd - s * sd, sn = s * cd + c * sd;
                    c = cn; s = sn;
                }
            };
            if (prev_v) add(prev.Ml[li], prev.PHIl[li], pw0l, tables::kWs + kN);
            if (cur_v) add(cur.Ml[li], cur.PHIl[li] - (cw0l * static_cast<float>(kN)), cw0l, tables::kWs);
        }
    }
    synthesize_unvoiced(out, cur, prev, noise);
    for (auto& v : out) v = std::clamp(v, -kSoftClip, kSoftClip);
}

// ---- トーン(原本 mbe_synthesizeTonef / mbe_renderTonef) ----
void Decoder::synthesize_tone(std::span<float, 160> out, const Bits49& d, Params& cur) {
    const int u0 = bits(d, 0, 12), u1 = bits(d, 12, 24), u3 = bits(d, 35, 49);
    const int AD = ((u0 & 0x3f) << 1) + ((u3 >> 4) & 0x1);
    const int ID1 = (u1 & 0xfff) >> 4;
    float f1 = 0.f, f2 = 0.f;
    if (ID1 == 5) f1 = f2 = 156.25f;
    else if (ID1 == 6) f1 = f2 = 187.5f;
    else if (ID1 == 128 || ID1 == 163) { f1 = 490.f; f2 = 350.f; }
    else if (ID1 >= 7 && ID1 <= 122) f1 = f2 = 31.25f * static_cast<float>(ID1);
    if (f1 <= 0.f) { std::fill(out.begin(), out.end(), 0.f); return; }
    const bool dual = f2 > 0.f && std::fabs(f2 - f1) > 1e-6f;
    const float gain = ((AD < 0 ? 0.f : static_cast<float>(AD)) / 127.f) * kSoftClip;
    auto step = [](double hz) { const double s = (hz / 8000.0) * 4294967296.0; return s <= 0.0 ? 0u : static_cast<uint32_t>(s + 0.5); };
    auto sample = [](uint32_t ph) { return std::sin(static_cast<float>((static_cast<double>(ph) * ((2.0 * std::numbers::pi) / 4294967296.0)) - (std::numbers::pi / 2.0))); };
    const uint32_t s1 = step(f1), s2 = dual ? step(f2) : 0u;
    uint32_t p1 = static_cast<uint32_t>(cur.swn), p2 = static_cast<uint32_t>(cur.un);
    for (int n = 0; n < kN; ++n) {
        p1 += s1;
        const float a = sample(p1);
        if (dual) { p2 += s2; out[static_cast<std::size_t>(n)] = 0.5f * gain * a + 0.5f * gain * sample(p2); }
        else out[static_cast<std::size_t>(n)] = gain * a;
    }
    cur.swn = static_cast<int>(p1); cur.un = static_cast<int>(p2);
}

// ---- 1 フレーム処理(原本 mbe_processAmbe2450Dataf_internal) ----
void Decoder::process(const Bits49& d, int c0_errors, int total_errors, bool c0_valid, std::span<float, 160> out) {
    cur_.errorCountTotal = total_errors;
    cur_.errorRate = (0.95f * prev_.errorRate) + (0.001064f * static_cast<float>(total_errors));

    const Kind kind = decode_params(d, total_errors);
    if (kind == Kind::Erasure) { cur_.repeat = 0; cur_.repeatCount = 0; erasure_params(cur_, prev_); }
    else if (kind == Kind::Tone) { cur_.repeat = 0; cur_.repeatCount = 0; }
    else {
        const bool repeat = c0_valid ? (c0_errors >= 4 || (c0_errors >= 2 && total_errors >= 6)) : (total_errors > 3);
        if (repeat) { cur_ = prev_; ++cur_.repeat; ++cur_.repeatCount; }
        else { cur_.repeat = 0; cur_.repeatCount = 0; }
    }

    if (kind == Kind::Voice) {
        if (cur_.repeat <= 3) {
            prev_ = cur_;
            spectral_enhance(cur_);
            synthesize_speech(out, cur_, prev_enh_);
            prev_enh_ = cur_;
        } else { comfort_noise(out); init_params(prev_); cur_ = prev_; prev_enh_ = prev_; }
    } else if (kind == Kind::Tone) {
        const int id1 = bits(d, 12, 20);
        const bool valid = id1 == 5 || id1 == 6 || (id1 >= 7 && id1 <= 122) || (id1 >= 128 && id1 <= 163);
        if (valid) synthesize_tone(out, d, cur_);
        else if (prev_.repeatCount < kMaxFrameRepeats) { Params synth = prev_enh_; synthesize_speech(out, synth, prev_enh_); prev_enh_ = synth; }
        else { comfort_noise(out); init_params(prev_); cur_ = prev_; prev_enh_ = prev_; }
    } else {   // Erasure: 白色雑音を出し、消失パラメータを前フレーム状態として保持
        comfort_noise(out);
        prev_ = cur_;
        prev_enh_ = cur_;
    }
}

} // namespace spear::std_t98::ambe
