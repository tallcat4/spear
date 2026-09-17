// Mode S のプリアンブル検出と PPM ビット判定(App 内 DSP、電力列 |x|² のみを見る。平方根を取らない)
//
// 時間構造(1 チップ = 0.5 µs、spc = 1 チップあたりのサンプル数 = in_rate / 2 MHz):
//   プリアンブル 8 µs = 16 チップ。パルスはチップ 0, 2, 7, 9(1.0 / 3.5 / 4.5 µs)、他は無音
//   データ 1 bit = 1 µs = 2 チップ。前半にパルスなら 1、後半なら 0(PPM)。56 または 112 bit
// 検出: 4 パルスチップの平均電力 > preamble_ratio × 無音チップの平均電力、かつ各パルスが平均の min_pulse_ratio 以上。
// spc 以内の最良位置に揃えてから bit を判定し、呼び出し側(CRC 判定)が受理したフレームの分だけ読み飛ばす。
// dump1090 の detectModeS(2 Msps、1 サンプル/チップ)を任意の整数 spc に一般化したもの。
#pragma once

#include "protocol/modes.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace spear::adsb {

inline constexpr int kPreambleChips = 16;
inline constexpr int kMaxFrameBits = 112;
inline constexpr int kWindowChips = kPreambleChips + 2 * kMaxFrameBits;   // 240 チップ = 120 µs

struct PpmConfig {
    int   spc = 4;                    // サンプル / チップ(8 Msps → 4)
    float preamble_ratio = 9.0f;      // パルス平均電力 / 無音平均電力 の下限(9 = 9.5 dB)
    float min_pulse_ratio = 0.16f;    // 各パルス電力 / パルス平均電力 の下限(0.16 = −8 dB)
};

struct Candidate {
    uint64_t   index = 0;             // プリアンブル先頭の電力列 index
    FrameBytes bytes{};
    int        nbits = 56;
    float      level = 0;             // ビットのパルス側の平均電力(サンプル単位)。RSSI 用
    float      noise = 0;             // ビットの無音側の平均電力
    float      min_confidence = 0;    // 最も曖昧な bit の |a−b|/(a+b)
    std::span<const float> window;    // プリアンブル先頭から kWindowChips × spc サンプル(コールバック中のみ有効)
};

class PpmDecoder {
public:
    explicit PpmDecoder(PpmConfig cfg);
    const PpmConfig& config() const { return cfg_; }
    void reset();
    void discard() { buf_.clear(); have_buf0_ = false; }   // 入力の不連続: 溜めた電力列を捨てる(統計は保つ)
    // 電力列を渡す(first_index = pwr[0] の通し番号)。候補ごとに accept を呼び、true(CRC OK)ならそのフレーム分を読み飛ばす
    void process(std::span<const float> pwr, uint64_t first_index, const std::function<bool(const Candidate&)>& accept);
    uint64_t preambles() const { return preambles_; }
    std::size_t window_samples() const { return static_cast<std::size_t>(kWindowChips * cfg_.spc); }

private:
    bool decode_at(std::size_t i, Candidate& c) const;
    PpmConfig cfg_;
    std::vector<float> buf_;      // 未処理の電力(先頭 = buf0_)
    std::vector<float> chip_;     // chip_[i] = buf_[i .. i+spc) の和
    std::vector<float> pulse_, gap_;   // pulse_[i] = パルスチップ 4 つの和、gap_[i] = 無音チップ 10 個の和(ベクトル化しやすい形)
    uint64_t buf0_ = 0;
    bool have_buf0_ = false;
    uint64_t preambles_ = 0;
};

} // namespace spear::adsb
