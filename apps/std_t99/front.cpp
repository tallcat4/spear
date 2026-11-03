#include "front.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace spear::std_t99 {

Front::Front(double in_rate, double shift_hz) : in_rate_(in_rate), shift_hz_(shift_hz) {
    const auto decim = static_cast<std::size_t>(std::llround(in_rate / kReceiverRate));
    if (decim < 1 || std::abs(static_cast<double>(decim) * kReceiverRate - in_rate) > 1e-6)
        throw std::invalid_argument("STD-T99: input rate must be a multiple of 100 kHz");
    // ÷4 の段を最後に置く(400 kHz の帯域表示を出すため)。4 で割り切れないレートは 1 段で 100 kHz へ
    std::size_t d2 = decim % 4 == 0 ? 4 : decim;
    std::size_t d1 = decim / d2;
    if (decim == 1) d1 = d2 = 1;
    if (d1 > 1) {
        const double r1 = in_rate / static_cast<double>(d1);
        d1_ = std::make_unique<dsp::FirDecimator<cf32>>("t99_resamp1", in_rate, dsp::design_lowpass(in_rate, r1 * 0.45, 127), d1);
        prov_ = prov_.then(d1_->info());
    }
    band_rate_ = in_rate / static_cast<double>(d1);
    if (d2 > 1) {
        d2_ = std::make_unique<dsp::FirDecimator<cf32>>("t99_resamp2", band_rate_, dsp::design_lowpass(band_rate_, 40e3, 63), d2);
        prov_ = prov_.then(d2_->info());
    }
    // 回転は複素数の漸化式(倍精度、ブロックごとに正規化)。4 Msps で毎サンプル cos/sin を呼ぶより軽い
    const double w = -2.0 * std::numbers::pi * shift_hz_ / in_rate_;
    step_ = {std::cos(w), std::sin(w)};
}

void Front::process(std::span<const cf32> in, std::vector<cf32>& band, std::vector<cf32>& out) {
    if (shift_hz_ != 0) {
        rot_.resize(in.size());
        std::complex<double> p = phasor_;
        for (std::size_t i = 0; i < in.size(); ++i) {
            rot_[i] = in[i] * cf32(static_cast<float>(p.real()), static_cast<float>(p.imag()));
            p *= step_;
        }
        phasor_ = p / std::abs(p);
        in = rot_;
    }
    if (d1_) {
        band.resize(in.size() / d1_->decim() + 2);
        band.resize(d1_->process(in, band));
    } else band.assign(in.begin(), in.end());
    if (d2_) {
        out.resize(band.size() / d2_->decim() + 2);
        out.resize(d2_->process(band, out));
    } else out = band;
}

} // namespace spear::std_t99
