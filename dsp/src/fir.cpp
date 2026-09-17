#include "spear/dsp/fir.hpp"

#include <cmath>
#include <numbers>

namespace spear::dsp {
namespace {
// 内積カーネル。累算器を 8 本に分けて和の順序を固定し、-ffast-math なしでも自動ベクトル化できる形にする
// (実測: 520 tap の実数 FIR 30 本 @62.5 kHz が素朴なループの ~5 倍速)。taps は時間順(h[0] が最古)。
inline float dot(const float* x, const float* h, std::size_t n) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
    std::size_t k = 0;
    for (; k + 8 <= n; k += 8) {
        a0 += x[k] * h[k]; a1 += x[k + 1] * h[k + 1]; a2 += x[k + 2] * h[k + 2]; a3 += x[k + 3] * h[k + 3];
        a4 += x[k + 4] * h[k + 4]; a5 += x[k + 5] * h[k + 5]; a6 += x[k + 6] * h[k + 6]; a7 += x[k + 7] * h[k + 7];
    }
    for (; k < n; ++k) a0 += x[k] * h[k];
    return ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7));
}
inline cf32 dot(const cf32* x, const float* h, std::size_t n) {
    // 複素 × 実: re/im を独立に。x を float 配列として見て h を 2 倍に展開したものと等価だが、ここは 4 本ずつ
    float r0 = 0, r1 = 0, r2 = 0, r3 = 0, i0 = 0, i1 = 0, i2 = 0, i3 = 0;
    std::size_t k = 0;
    for (; k + 4 <= n; k += 4) {
        r0 += x[k].real() * h[k]; i0 += x[k].imag() * h[k];
        r1 += x[k + 1].real() * h[k + 1]; i1 += x[k + 1].imag() * h[k + 1];
        r2 += x[k + 2].real() * h[k + 2]; i2 += x[k + 2].imag() * h[k + 2];
        r3 += x[k + 3].real() * h[k + 3]; i3 += x[k + 3].imag() * h[k + 3];
    }
    for (; k < n; ++k) { r0 += x[k].real() * h[k]; i0 += x[k].imag() * h[k]; }
    return {(r0 + r1) + (r2 + r3), (i0 + i1) + (i2 + i3)};
}
} // namespace

std::vector<float> design_lowpass(double sample_rate, double cutoff_hz, std::size_t taps) {
    if (taps % 2 == 0) ++taps;
    std::vector<float> h(taps);
    const double fc = cutoff_hz / sample_rate;           // 正規化 (cycles/sample)
    const double m = static_cast<double>(taps - 1) / 2.0;
    double sum = 0;
    for (std::size_t n = 0; n < taps; ++n) {
        const double k = static_cast<double>(n) - m;
        const double sinc = k == 0 ? 2.0 * fc : std::sin(2.0 * std::numbers::pi * fc * k) / (std::numbers::pi * k);
        const double w = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(taps - 1));
        h[n] = static_cast<float>(sinc * w);
        sum += h[n];
    }
    for (auto& v : h) v = static_cast<float>(v / sum);   // DC 利得 1
    return h;
}

template <class T>
FirDecimator<T>::FirDecimator(std::string name, double in_rate, std::vector<float> taps, std::size_t decim)
    : taps_(std::move(taps)), decim_(decim == 0 ? 1 : decim) {
    taps_rev_.assign(taps_.rbegin(), taps_.rend());   // 畳み込み用に時間順(最古が先頭)
    info_.name = std::move(name);
    info_.in_rate = in_rate;
    info_.out_rate = in_rate / static_cast<double>(decim_);
    info_.group_delay_in = static_cast<double>(taps_.size() - 1) / 2.0;   // 線形位相 FIR
    reset();
}

template <class T>
void FirDecimator<T>::reset() {
    hist_.assign(taps_.size() > 0 ? taps_.size() - 1 : 0, T{});
    phase_ = 0;
}

template <class T>
std::size_t FirDecimator<T>::process(std::span<const T> in, std::span<T> out) {
    // 作業バッファ: 履歴 + 入力 を連結して畳み込む(素朴だが 2 Msps では十分。最適化対象 §1.1)
    const std::size_t nt = taps_.size();
    auto& buf = buf_;
    buf.clear();
    buf.reserve(hist_.size() + in.size());
    buf.insert(buf.end(), hist_.begin(), hist_.end());
    buf.insert(buf.end(), in.begin(), in.end());
    std::size_t produced = 0;
    // 出力 sample は入力位置 i(= buf[i + nt - 1] が最新)で計算。phase_ で間引き位相を保つ
    std::size_t i = phase_;
    for (; i < in.size(); i += decim_) {
        if (produced < out.size()) out[produced++] = dot(buf.data() + i, taps_rev_.data(), nt);
    }
    phase_ = i - in.size();
    // 履歴更新
    if (nt > 1) {
        const std::size_t keep = nt - 1;
        hist_.assign(buf.end() - static_cast<long>(keep), buf.end());
    }
    return produced;
}

template class FirDecimator<cf32>;
template class FirDecimator<float>;

template <class T>
FirInterpolator<T>::FirInterpolator(std::string name, double in_rate, std::vector<float> taps, std::size_t interp)
    : interp_(interp == 0 ? 1 : interp) {
    while (taps.size() % interp_ != 0) taps.push_back(0.f);
    taps_per_phase_ = taps.size() / interp_;
    phase_taps_.assign(interp_, std::vector<float>(taps_per_phase_, 0.f));
    for (std::size_t k = 0; k < taps_per_phase_; ++k)
        for (std::size_t p = 0; p < interp_; ++p)
            phase_taps_[p][k] = taps[k * interp_ + p] * static_cast<float>(interp_);
    phase_taps_rev_.resize(interp_);
    for (std::size_t p = 0; p < interp_; ++p) phase_taps_rev_[p].assign(phase_taps_[p].rbegin(), phase_taps_[p].rend());
    info_.name = std::move(name);
    info_.in_rate = in_rate;
    info_.out_rate = in_rate * static_cast<double>(interp_);
    // 群遅延: 補間後 sample で (N-1)/2 → 入力 sample では /interp
    info_.group_delay_in = static_cast<double>(taps.size() - 1) / 2.0 / static_cast<double>(interp_);
    reset();
}

template <class T>
void FirInterpolator<T>::reset() {
    hist_.assign(taps_per_phase_ > 0 ? taps_per_phase_ - 1 : 0, T{});
}

template <class T>
std::size_t FirInterpolator<T>::process(std::span<const T> in, std::span<T> out) {
    auto& buf = buf_;
    buf.clear();
    buf.reserve(hist_.size() + in.size());
    buf.insert(buf.end(), hist_.begin(), hist_.end());
    buf.insert(buf.end(), in.begin(), in.end());
    std::size_t produced = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const T* p = buf.data() + i;   // p[taps_per_phase_-1] が最新
        for (std::size_t ph = 0; ph < interp_; ++ph) {
            if (produced < out.size()) out[produced++] = dot(p, phase_taps_rev_[ph].data(), taps_per_phase_);
        }
    }
    if (taps_per_phase_ > 1) hist_.assign(buf.end() - static_cast<long>(taps_per_phase_ - 1), buf.end());
    return produced;
}

template class FirInterpolator<cf32>;
template class FirInterpolator<float>;

} // namespace spear::dsp
