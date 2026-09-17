#include "receiver.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spear::adsb {

Receiver::Receiver(ReceiverConfig cfg) : cfg_(cfg) {
    // spc は整数でなければならない(2 MHz の整数倍のレート)。合わなければ最も近い整数に丸めて動かす(検出は劣化する)
    const int spc = std::max(1, static_cast<int>(std::llround(cfg_.in_rate / 2e6)));
    PpmConfig pc;
    pc.spc = spc;
    pc.preamble_ratio = static_cast<float>(std::pow(10.0, cfg_.preamble_ratio_db / 10));
    pc.min_pulse_ratio = static_cast<float>(std::pow(10.0, cfg_.min_pulse_db / 10));
    ppm_ = std::make_unique<PpmDecoder>(pc);
    lowpass_ = std::make_unique<dsp::FirDecimator<cf32>>("adsb_lowpass", cfg_.in_rate,
        dsp::design_lowpass(cfg_.in_rate, cfg_.lowpass_cutoff_hz, static_cast<std::size_t>(cfg_.lowpass_taps)), 1);
    prov_ = dsp::Provenance::identity().then(lowpass_->info());
    syndromes_ = single_bit_syndromes(112);
    const double w = -2.0 * std::numbers::pi * cfg_.lo_offset_hz / cfg_.in_rate;
    step_ = cf32(static_cast<float>(std::cos(w)), static_cast<float>(std::sin(w)));
}

void Receiver::process(std::span<const cf32> in, uint64_t first_index) {
    // provenance の原点。入力 index が連続でなければ(欠落 / generation 切替)取り直し、途中の振幅列は捨てる
    const int64_t offset = static_cast<int64_t>(first_index) - static_cast<int64_t>(mag_index_);
    if (!have_offset_ || offset != in_offset_) { if (have_offset_) ppm_->discard(); in_offset_ = offset; have_offset_ = true; }
    // 1) 回転: 1090 MHz(IF = +lo_offset)を 0 Hz へ。位相はブロックをまたいで連続、振幅は毎ブロック正規化。
    //    8 サンプル分の位相子を先に作り、位相子は 8 サンプルごとに進める(1 サンプルごとの直列乗算より 4 倍速)。
    //    複素乗算は float で手書き(std::complex の operator* は NaN 処理の __mulsc3 に落ちて遅い)。
    rot_.resize(in.size());
    if (cfg_.lo_offset_hz != 0) {
        constexpr std::size_t V = 8;
        float pr = phasor_.real(), pi = phasor_.imag();
        const float sr = step_.real(), si = step_.imag();
        float vr[V], vi[V];   // step^k
        vr[0] = 1.f; vi[0] = 0.f;
        for (std::size_t k = 1; k < V; ++k) { vr[k] = vr[k - 1] * sr - vi[k - 1] * si; vi[k] = vr[k - 1] * si + vi[k - 1] * sr; }
        const float s8r = vr[V - 1] * sr - vi[V - 1] * si, s8i = vr[V - 1] * si + vi[V - 1] * sr;
        const float* xf = reinterpret_cast<const float*>(in.data());
        float* yf = reinterpret_cast<float*>(rot_.data());
        std::size_t i = 0;
        for (; i + V <= in.size(); i += V) {
            for (std::size_t k = 0; k < V; ++k) {
                const float cr = pr * vr[k] - pi * vi[k], ci = pr * vi[k] + pi * vr[k];
                const float ar = xf[2 * (i + k)], ai = xf[2 * (i + k) + 1];
                yf[2 * (i + k)] = ar * cr - ai * ci;
                yf[2 * (i + k) + 1] = ar * ci + ai * cr;
            }
            const float nr = pr * s8r - pi * s8i, ni = pr * s8i + pi * s8r;
            pr = nr; pi = ni;
        }
        for (; i < in.size(); ++i) {
            const float ar = xf[2 * i], ai = xf[2 * i + 1];
            yf[2 * i] = ar * pr - ai * pi;
            yf[2 * i + 1] = ar * pi + ai * pr;
            const float nr = pr * sr - pi * si, ni = pr * si + pi * sr;
            pr = nr; pi = ni;
        }
        const float mag = std::sqrt(pr * pr + pi * pi);
        phasor_ = cf32(pr / mag, pi / mag);
    } else {
        std::copy(in.begin(), in.end(), rot_.begin());
    }
    // 2) 低域 FIR(decim 1)
    filt_.resize(in.size() + 2);
    const std::size_t n = lowpass_->process(rot_, filt_);
    // 3) 電力 |x|²(平方根は取らない。閾値は電力比で持つ)
    pwr_.resize(n);
    for (std::size_t i = 0; i < n; ++i) pwr_[i] = std::norm(filt_[i]);
    const uint64_t mag0 = mag_index_;
    mag_index_ += n;
    if (obs_.power) obs_.power(std::span<const float>(pwr_.data(), n), mag0);
    // 4) プリアンブル検出 → ビット → CRC
    ppm_->process(std::span<const float>(pwr_.data(), n), mag0, [this](const Candidate& c) { return accept(c); });
    metrics_.preambles = ppm_->preambles();
    // 既知 ICAO の掃除
    const double now = static_cast<double>(mag_index_) / cfg_.in_rate;
    if (now - last_prune_s_ > 30) {
        last_prune_s_ = now;
        for (auto it = known_icao_.begin(); it != known_icao_.end();) it = (now - it->second > cfg_.icao_cache_s) ? known_icao_.erase(it) : std::next(it);
    }
    metrics_.known_icao = known_icao_.size();
}

bool Receiver::accept(const Candidate& c) {
    Frame f;
    f.bytes = c.bytes;
    f.nbits = c.nbits;
    f.msg = decode(f.bytes, f.nbits);
    const double t = static_cast<double>(c.index) / cfg_.in_rate;
    if (f.msg.df == 17 || f.msg.df == 18) {
        if (!f.msg.crc_ok && cfg_.fix_single_bit && fix_single_bit(f.bytes, f.nbits, f.msg.remainder, syndromes_)) {
            f.msg = decode(f.bytes, f.nbits);
            f.fixed = f.msg.crc_ok;
        }
        if (!f.msg.crc_ok) { if (f.nbits == 112) ++metrics_.crc_bad; return false; }
        // 訂正したフレームは ICAO が既知のときだけ信用する(112 本のシンドロームに偶然一致する確率は候補あたり 112/2^24)
        if (f.fixed && !known_icao_.count(f.msg.icao)) { ++metrics_.crc_bad; return false; }
        f.accepted = true;
        known_icao_[f.msg.icao] = t;
    } else if (f.msg.df == 11) {
        if (!f.msg.crc_ok) return false;
        f.accepted = true;
        known_icao_[f.msg.icao] = t;
    } else if (f.msg.icao_from_ap) {
        // AP 形式: 剰余 = ICAO。既知アドレス(寿命内)に一致するときだけ受理
        auto it = known_icao_.find(f.msg.icao);
        if (it == known_icao_.end() || t - it->second > cfg_.icao_cache_s) { ++metrics_.ap_unknown; return false; }
        f.accepted = true;
        it->second = t;
    } else {
        return false;
    }
    if (f.fixed) ++metrics_.fixed;
    ++metrics_.frames;
    ++metrics_.by_df[static_cast<std::size_t>(f.msg.df & 31)];
    // provenance: 電力列 index → radio.rx index(FIR の群遅延を引く)
    const double in_idx = prov_.input_index(static_cast<double>(c.index));
    f.input_sample_index = static_cast<uint64_t>(std::max<int64_t>(0, in_offset_ + std::llround(in_idx)));
    f.input_sample_end = f.input_sample_index + static_cast<uint64_t>((kPreambleChips + 2 * f.nbits) * samples_per_chip());
    f.t_s = t;
    f.rssi_db = 10.0 * std::log10(static_cast<double>(c.level) + 1e-20);
    f.snr_db = 10.0 * std::log10((static_cast<double>(c.level) + 1e-20) / (static_cast<double>(c.noise) + 1e-20));
    if (obs_.frame) obs_.frame(f, c.window);
    return true;
}

} // namespace spear::adsb
