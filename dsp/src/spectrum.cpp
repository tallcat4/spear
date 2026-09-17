#include "spear/dsp/spectrum.hpp"

#include <cmath>
#include <fftw3.h>
#include <numbers>

#include "spear/dsp/fftw_planner.hpp"

namespace spear::dsp {

using detail::fftw_planner_mutex;

SpectrumEstimator::SpectrumEstimator(std::size_t nfft, unsigned flags) : n_(nfft), window_(nfft), acc_(nfft) {
    in_  = fftwf_alloc_complex(nfft);
    out_ = fftwf_alloc_complex(nfft);
    {
        std::lock_guard lk(fftw_planner_mutex());
        plan_ = fftwf_plan_dft_1d(static_cast<int>(nfft), static_cast<fftwf_complex*>(in_),
                                  static_cast<fftwf_complex*>(out_), FFTW_FORWARD, flags ? flags : FFTW_MEASURE);
    }
    // Hann 窓。coherent gain で正規化し、full-scale tone が 0 dBFS になるようにする。
    double sum = 0;
    for (std::size_t i = 0; i < nfft; ++i) {
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(nfft)));
        sum += window_[i];
    }
    window_gain_ = static_cast<float>(sum);
}

SpectrumEstimator::~SpectrumEstimator() {
    {
        std::lock_guard lk(fftw_planner_mutex());
        fftwf_destroy_plan(static_cast<fftwf_plan>(plan_));
    }
    fftwf_free(in_);
    fftwf_free(out_);
}

void SpectrumEstimator::run(std::vector<float>& db_out, bool accumulate) {
    fftwf_execute(static_cast<fftwf_plan>(plan_));
    auto* o = static_cast<fftwf_complex*>(out_);
    const float norm = 1.f / (window_gain_ * window_gain_);
    if (!accumulate) db_out.assign(n_, 0.f);
    for (std::size_t i = 0; i < n_; ++i) {
        const std::size_t k = (i + n_ / 2) % n_; // fftshift
        db_out[i] += (o[k][0] * o[k][0] + o[k][1] * o[k][1]) * norm;
    }
}

bool SpectrumEstimator::compute(std::span<const sc16> iq, std::vector<float>& db_out) {
    if (iq.size() < n_) return false;
    auto* in = static_cast<fftwf_complex*>(in_);
    for (std::size_t i = 0; i < n_; ++i) {
        in[i][0] = iq[i].real() / 32768.f * window_[i];
        in[i][1] = iq[i].imag() / 32768.f * window_[i];
    }
    run(db_out, false);
    for (auto& v : db_out) v = 10.f * std::log10(v + 1e-20f);
    return true;
}

bool SpectrumEstimator::compute(std::span<const cf32> iq, std::vector<float>& db_out) {
    if (iq.size() < n_) return false;
    auto* in = static_cast<fftwf_complex*>(in_);
    for (std::size_t i = 0; i < n_; ++i) {
        in[i][0] = iq[i].real() * window_[i];
        in[i][1] = iq[i].imag() * window_[i];
    }
    run(db_out, false);
    for (auto& v : db_out) v = 10.f * std::log10(v + 1e-20f);
    return true;
}

bool SpectrumEstimator::compute_averaged(std::span<const sc16> iq, std::vector<float>& db_out) {
    const std::size_t frames = iq.size() / n_;
    if (frames == 0) return false;
    db_out.assign(n_, 0.f);
    auto* in = static_cast<fftwf_complex*>(in_);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t i = 0; i < n_; ++i) {
            in[i][0] = iq[f * n_ + i].real() / 32768.f * window_[i];
            in[i][1] = iq[f * n_ + i].imag() / 32768.f * window_[i];
        }
        run(db_out, true);
    }
    const float inv = 1.f / static_cast<float>(frames);
    for (auto& v : db_out) v = 10.f * std::log10(v * inv + 1e-20f);
    return true;
}

bool SpectrumEstimator::compute_averaged(std::span<const cf32> iq, std::vector<float>& db_out) {
    const std::size_t frames = iq.size() / n_;
    if (frames == 0) return false;
    db_out.assign(n_, 0.f);
    auto* in = static_cast<fftwf_complex*>(in_);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t i = 0; i < n_; ++i) {
            in[i][0] = iq[f * n_ + i].real() * window_[i];
            in[i][1] = iq[f * n_ + i].imag() * window_[i];
        }
        run(db_out, true);
    }
    const float inv = 1.f / static_cast<float>(frames);
    for (auto& v : db_out) v = 10.f * std::log10(v * inv + 1e-20f);
    return true;
}

} // namespace spear::dsp
