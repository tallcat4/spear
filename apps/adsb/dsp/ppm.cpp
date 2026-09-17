#include "ppm.hpp"

#include <algorithm>
#include <cmath>

namespace spear::adsb {

namespace {
// プリアンブルのパルスチップと無音チップ(チップ 10 は 9 のパルスの裾、15 はデータ直前なので判定に使わない)
constexpr int kPulseChips[] = {0, 2, 7, 9};
constexpr int kGapChips[] = {1, 3, 4, 5, 6, 8, 11, 12, 13, 14};
constexpr int kNumPulse = 4, kNumGap = 10;
}

PpmDecoder::PpmDecoder(PpmConfig cfg) : cfg_(cfg) {
    if (cfg_.spc < 1) cfg_.spc = 1;
}

void PpmDecoder::reset() {
    buf_.clear();
    chip_.clear();
    have_buf0_ = false;
    preambles_ = 0;
}

void PpmDecoder::process(std::span<const float> pwr, uint64_t first_index, const std::function<bool(const Candidate&)>& accept) {
    // 途切れ(index の不連続)は状態を捨てる
    if (have_buf0_ && buf0_ + buf_.size() != first_index) { buf_.clear(); have_buf0_ = false; }
    if (!have_buf0_) { buf0_ = first_index; have_buf0_ = true; }
    buf_.insert(buf_.end(), pwr.begin(), pwr.end());
    const std::size_t spc = static_cast<std::size_t>(cfg_.spc);
    const std::size_t need = window_samples();
    if (buf_.size() < need) return;

    // チップ和(移動和。float の累積誤差は 4096 サンプルごとに正確に取り直す)
    const std::size_t nchip = buf_.size() - spc + 1;
    chip_.resize(nchip);
    {
        float s = 0;
        for (std::size_t k = 0; k < spc; ++k) s += buf_[k];
        chip_[0] = s;
        for (std::size_t i = 1; i < nchip; ++i) {
            if (i % 4096 == 0) { s = 0; for (std::size_t k = 0; k < spc; ++k) s += buf_[i + k]; }
            else s += buf_[i + spc - 1] - buf_[i - 1];
            chip_[i] = s;
        }
    }
    // パルス和・無音和を配列で作る(ずらした配列の和 → コンパイラがベクトル化する)
    const std::size_t last = buf_.size() - need;   // この位置までは全窓が見える
    const std::size_t nscan = last + 1;
    pulse_.resize(nscan);
    gap_.resize(nscan);
    {
        const float* c = chip_.data();
        float* p = pulse_.data();
        float* g = gap_.data();
        const std::size_t o0 = 0, o2 = 2 * spc, o7 = 7 * spc, o9 = 9 * spc;
        for (std::size_t i = 0; i < nscan; ++i) p[i] = c[i + o0] + c[i + o2] + c[i + o7] + c[i + o9];
        const std::size_t g1 = spc, g3 = 3 * spc, g4 = 4 * spc, g5 = 5 * spc, g6 = 6 * spc, g8 = 8 * spc, g11 = 11 * spc, g12 = 12 * spc, g13 = 13 * spc, g14 = 14 * spc;
        for (std::size_t i = 0; i < nscan; ++i)
            g[i] = c[i + g1] + c[i + g3] + c[i + g4] + c[i + g5] + c[i + g6] + c[i + g8] + c[i + g11] + c[i + g12] + c[i + g13] + c[i + g14];
    }
    auto chip = [&](std::size_t i, int c) { return chip_[i + static_cast<std::size_t>(c) * spc]; };
    const float kRatio = cfg_.preamble_ratio * static_cast<float>(kNumPulse) / static_cast<float>(kNumGap);   // pulse_ > kRatio × gap_ ⇔ 平均の比

    std::size_t i = 0;
    while (i <= last) {
        const float p = pulse_[i];
        if (p <= 0 || p < kRatio * gap_[i]) { ++i; continue; }
        const float pm = p / kNumPulse;
        bool pulses_ok = true;
        for (int c : kPulseChips) if (chip(i, c) < cfg_.min_pulse_ratio * pm) { pulses_ok = false; break; }
        if (!pulses_ok) { ++i; continue; }
        // spc 以内でパルス和が最大の位置に揃える
        std::size_t best = i;
        float bestp = p;
        for (std::size_t k = 1; k < spc && i + k <= last; ++k) {
            const float pk = pulse_[i + k];
            if (pk > bestp) { bestp = pk; best = i + k; }
        }
        ++preambles_;
        Candidate c;
        if (decode_at(best, c)) {
            c.index = buf0_ + best;
            c.window = std::span<const float>(buf_.data() + best, need);
            if (accept(c)) { i = best + static_cast<std::size_t>(kPreambleChips + 2 * c.nbits) * spc; continue; }
        }
        i = best + spc;   // 同じプリアンブルを別の位相で再試行しない
    }
    // 見終えた位置より前は捨てる(i ≤ buf_.size() が常に成り立つ: 読み飛ばしはフレーム = 窓以下、走査は last+1 まで)
    if (i > 0) { buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(i)); buf0_ += i; }
}

bool PpmDecoder::decode_at(std::size_t i, Candidate& c) const {
    const std::size_t spc = static_cast<std::size_t>(cfg_.spc);
    const std::size_t m = i + static_cast<std::size_t>(kPreambleChips) * spc;
    c.bytes.fill(0);
    double level = 0, noise = 0;
    float minconf = 1.f;
    int nbits = kMaxFrameBits;
    for (int k = 0; k < nbits; ++k) {
        const float a = chip_[m + static_cast<std::size_t>(2 * k) * spc];
        const float b = chip_[m + static_cast<std::size_t>(2 * k + 1) * spc];
        const bool bit = a > b;
        if (bit) c.bytes[static_cast<std::size_t>(k / 8)] |= static_cast<uint8_t>(0x80u >> (k % 8));
        level += std::max(a, b); noise += std::min(a, b);
        const float conf = std::fabs(a - b) / (a + b + 1e-12f);
        minconf = std::min(minconf, conf);
        if (k == 4) nbits = frame_bits_for_df(c.bytes[0] >> 3);   // DF が分かったら長さを確定
    }
    c.nbits = nbits;
    c.level = static_cast<float>(level / nbits / static_cast<double>(spc));
    c.noise = static_cast<float>(noise / nbits / static_cast<double>(spc));
    c.min_confidence = minconf;
    return true;
}

} // namespace spear::adsb
