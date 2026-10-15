#include "receiver.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spear::slpr {

// ---- チャネルごとの状態 ----
struct Receiver::Channel {
    int bin = 0;
    cf32 quad_prev{0.f, 0.f};
    float quad_gain = 1.f;
    dsp::FirDecimator<float> lpf;         // 音声 LPF(decim 1)
    float hp_a = 0.f, hp_lp = 0.f;        // 一次 DC 阻止: out = x − lp(x)
    std::vector<float> disc, audio;
    bool open = false;
    double opening_s = 0;                 // 開く閾値を超え続けている時間(attack)
    uint64_t opening_index = 0;           // 超え始めたブロックの radio.rx index(provenance: 開の時刻はここ)
    double closing_s = 0;                 // 閉じる閾値を下回り続けている時間(hold)
};

Receiver::Receiver(ReceiverConfig cfg) : cfg_(cfg) {
    const double post1 = band_rate();   // 400 kHz
    const auto decim1 = static_cast<std::size_t>(std::llround(cfg_.in_rate / post1));
    // resamp1: 通過 ±180 kHz(20 ch は ±131 kHz)。LO を帯域中心から 250 kHz 離すので、DC スパイクは −250 kHz = 阻止域(÷10 の折り返し先 +150 kHz は
    // bin +12 で、ch20(bin +10)より外)。127 tap は遷移 ~50 kHz で −250 kHz を 50 dB 以上落とす(4 Msps × 127 tap ÷ 10 は 1 コアの数 %)
    resamp1_ = std::make_unique<dsp::FirDecimator<cf32>>("resamp1", cfg_.in_rate,
        dsp::design_lowpass(cfg_.in_rate, post1 * 0.45, 127), decim1);
    // PFB プロトタイプ: cutoff = bin/2(6.25 kHz)、branch あたり 16 tap(遷移 ~2.6 kHz、阻止 −53 dB)。隣接チャネル(±12.5 kHz)の
    // 占有帯域(±5.5 kHz)は 7 kHz 以遠なので阻止域に入る。STD-T98 の 8 tap/branch では隣接の強い局が漏れて空きチャネルを開けてしまう
    pfb_ = std::make_unique<dsp::PfbChannelizer>(static_cast<std::size_t>(cfg_.pfb_channels), post1,
        dsp::design_lowpass(post1, kSpacingHz / 2.0, static_cast<std::size_t>(cfg_.pfb_channels) * 16 - 1));

    const auto lpf_taps = dsp::design_lowpass(kSpacingHz, cfg_.audio_cutoff_hz, 63);
    for (int c = 0; c < kNumChannels; ++c) {
        auto ch = std::make_unique<Channel>();
        const int g = kChannels[static_cast<std::size_t>(c)].grid;
        ch->bin = g >= 0 ? g : cfg_.pfb_channels + g;
        ch->quad_gain = static_cast<float>(kSpacingHz / (2.0 * std::numbers::pi * cfg_.fm_dev_hz));
        ch->lpf = dsp::FirDecimator<float>("audio_lpf", kSpacingHz, lpf_taps, 1);
        ch->hp_a = static_cast<float>(std::exp(-2.0 * std::numbers::pi * cfg_.audio_hp_hz / kSpacingHz));
        ch_.push_back(std::move(ch));
    }
    metrics_.assign(static_cast<std::size_t>(kNumChannels), {});
    // provenance: 12.5 kHz sample → in index(resamp1 → pfb)。音声 LPF は同レートなので index は変わらない(群遅延 31 sample = 2.5 ms は無視)
    prov_chan_ = dsp::Provenance::identity().then(resamp1_->info()).then(pfb_->info());
}

Receiver::~Receiver() = default;

int Receiver::channel_to_bin(int ch) const { return ch_[static_cast<std::size_t>(ch)]->bin; }

void Receiver::process(std::span<const cf32> in, uint64_t first_index) {
    if (!have_in0_) { in0_ = first_index; have_in0_ = true; }
    // 0) 帯域中心を DC へ(ブロック境界も rot_phase_ で位相連続)
    if (cfg_.freq_err_hz != 0) {
        rot_.resize(in.size());
        const double step = -2.0 * std::numbers::pi * cfg_.freq_err_hz / cfg_.in_rate;
        for (std::size_t i = 0; i < in.size(); ++i) {
            rot_[i] = in[i] * cf32(static_cast<float>(std::cos(rot_phase_)), static_cast<float>(std::sin(rot_phase_)));
            rot_phase_ += step;
            if (rot_phase_ > std::numbers::pi) rot_phase_ -= 2 * std::numbers::pi; else if (rot_phase_ < -std::numbers::pi) rot_phase_ += 2 * std::numbers::pi;
        }
        in = rot_;
    }
    // 1) resamp1
    buf1_.resize(in.size() / resamp1_->decim() + 2);
    const std::size_t n1 = resamp1_->process(in, buf1_);
    if (obs_.band) obs_.band(std::span<const cf32>(buf1_.data(), n1), band_index_);
    band_index_ += n1;
    // 2) PFB
    for (auto& b : bins_) b.clear();
    pfb_->process(std::span<const cf32>(buf1_.data(), n1), bins_);
    const std::size_t n = bins_.empty() ? 0 : bins_[0].size();   // 全 bin 同数
    if (n == 0) return;
    // 3a) 全チャネルのブロック電力と床(中央値)。床は全チャネル共通 — AGC のゲイン変化や広帯域雑音は全チャネルに同じだけ乗る
    floor_tmp_.clear();
    for (int c = 0; c < kNumChannels; ++c) {
        const auto& bin = bins_[static_cast<std::size_t>(ch_[static_cast<std::size_t>(c)]->bin)];
        double pw = 0;
        for (auto x : bin) pw += std::norm(x);
        metrics_[static_cast<std::size_t>(c)].power_db = 10.0 * std::log10(pw / static_cast<double>(n) + 1e-20);
        floor_tmp_.push_back(metrics_[static_cast<std::size_t>(c)].power_db);
    }
    std::nth_element(floor_tmp_.begin(), floor_tmp_.begin() + static_cast<long>(floor_tmp_.size() / 2), floor_tmp_.end());
    const double floor_db = floor_tmp_[floor_tmp_.size() / 2];
    const double thr_open = floor_db + cfg_.squelch_margin_db, thr_close = thr_open - cfg_.squelch_hyst_db;
    const double dt = static_cast<double>(n) / kSpacingHz;
    const uint64_t input_index = in0_ + static_cast<uint64_t>(std::max(0.0, prov_chan_.input_index(static_cast<double>(chan_index_))));
    // 3b) チャネルごと
    for (int c = 0; c < kNumChannels; ++c) {
        auto& ch = *ch_[static_cast<std::size_t>(c)];
        auto& m = metrics_[static_cast<std::size_t>(c)];
        const auto& bin = bins_[static_cast<std::size_t>(ch.bin)];
        // squelch: 開くのは thr_open を attack_s 超え続けたとき、閉じるのは thr_close を hold_s 下回り続けたとき
        const bool was = ch.open;
        if (m.power_db >= thr_open) {
            ch.closing_s = 0;
            if (!ch.open) {
                if (ch.opening_s == 0) ch.opening_index = input_index;
                ch.opening_s += dt;
                if (ch.opening_s >= cfg_.squelch_attack_s) ch.open = true;
            }
        } else {
            ch.opening_s = 0;
            if (ch.open && m.power_db < thr_close) { ch.closing_s += dt; if (ch.closing_s >= cfg_.squelch_hold_s) { ch.open = false; ch.closing_s = 0; } }
            else ch.closing_s = 0;
        }
        if (ch.open != was) {
            if (ch.open) { ++m.opens; ch.opening_s = 0; }
            // provenance (§3.6): 開は超え始めたブロックの先頭、閉はこのブロックの先頭(radio.rx index)
            if (obs_.squelch) obs_.squelch(c, ch.open, ch.open ? ch.opening_index : input_index);
        }
        if (ch.open) m.open_s += dt;
        m.open = ch.open;
        m.floor_db = floor_db;
        m.threshold_db = thr_open;
        if (obs_.channel_iq) obs_.channel_iq(c, bin, chan_index_);
        // 4) quadrature demod(閉でも回す: 開いた瞬間から DC 阻止と LPF の状態が定常になっている)
        ch.disc.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const cf32 x = bin[i];
            const cf32 p = x * std::conj(ch.quad_prev);
            ch.quad_prev = x;
            ch.disc[i] = std::atan2(p.imag(), p.real()) * ch.quad_gain;
        }
        if (obs_.discriminator) obs_.discriminator(c, std::span<const float>(ch.disc.data(), n));
        // 5) 音声 LPF → DC 阻止 → ゲート → クランプ
        ch.audio.resize(n + 1);
        const std::size_t na = ch.lpf.process(std::span<const float>(ch.disc.data(), n), ch.audio);
        double apw = 0;
        for (std::size_t i = 0; i < na; ++i) {
            const float x = ch.audio[i];
            ch.hp_lp = ch.hp_a * ch.hp_lp + (1.f - ch.hp_a) * x;
            float v = ch.open ? x - ch.hp_lp : 0.f;
            v = std::clamp(v, -1.f, 1.f);
            ch.audio[i] = v;
            apw += static_cast<double>(v) * v;
        }
        m.audio_db = 10.0 * std::log10(apw / static_cast<double>(std::max<std::size_t>(na, 1)) + 1e-20);
        if (obs_.audio) obs_.audio(c, std::span<const float>(ch.audio.data(), na), chan_index_);
    }
    chan_index_ += n;
}

} // namespace spear::slpr
