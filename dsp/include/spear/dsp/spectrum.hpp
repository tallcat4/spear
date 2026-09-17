// S.P.E.A.R. dsp — Spectrum estimator
// sc16 IQ block → power spectrum [dBFS] (fftshift 済み)。view frame (§9.2) の生成に使う。
#pragma once

#include "spear/core/types.hpp"

#include <span>
#include <vector>

namespace spear::dsp {

class SpectrumEstimator {
public:
    explicit SpectrumEstimator(std::size_t nfft, unsigned flags = 0);
    ~SpectrumEstimator();
    SpectrumEstimator(const SpectrumEstimator&) = delete;
    SpectrumEstimator& operator=(const SpectrumEstimator&) = delete;

    std::size_t nfft() const noexcept { return n_; }

    // 先頭 nfft sample を使う。sample 数が不足なら false。
    bool compute(std::span<const sc16> iq, std::vector<float>& db_out);
    bool compute(std::span<const cf32> iq, std::vector<float>& db_out);

    // 複数 FFT の平均 (power average)。block 全体を使う。
    bool compute_averaged(std::span<const sc16> iq, std::vector<float>& db_out);
    bool compute_averaged(std::span<const cf32> iq, std::vector<float>& db_out);

    // bin index → 周波数 offset [Hz] (fftshift 済み座標)
    static double bin_to_offset(std::size_t bin, std::size_t nfft, double sample_rate) {
        return (static_cast<double>(bin) - static_cast<double>(nfft) / 2.0) * sample_rate / static_cast<double>(nfft);
    }

private:
    void run(std::vector<float>& db_out, bool accumulate);
    std::size_t n_;
    std::vector<float> window_;
    float window_gain_ = 1.f;
    void* plan_ = nullptr;
    void* in_ = nullptr;
    void* out_ = nullptr;
    std::vector<float> acc_;
};

} // namespace spear::dsp
