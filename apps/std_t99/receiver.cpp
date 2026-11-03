#include "receiver.hpp"

#include "std_t98/dsp/rrc.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace spear::std_t99 {

bool Frame::crc_ok() const {
    if (sacch.crc_ok) return true;
    for (const auto& h : halves)
        if (h.control && h.control->crc_ok) return true;
    return false;
}

bool Frame::control_crc_ok() const {
    for (const auto& h : halves)
        if (h.control && h.control->crc_ok) return true;
    return false;
}

bool Frame::plausible() const {
    const bool rich_ok = rich.parity_ok && (rich.f == 0b01 || rich.f == 0b10) && (rich.m == 0b01 || rich.m == 0b10);
    return rich_ok || control_crc_ok();
}

Frame decode_frame(std::span<const float> raw) {
    Frame f;
    f.raw.assign(raw.begin(), raw.end());
    f.fields = split_frame(symbols_to_bits(dewhiten(quantize(raw))));
    f.rich = decode_rich(f.fields.rich);
    f.sacch = decode_sacch(f.fields.sacch);
    // Table Annex 2-4-7: which half carries TCH. F=01 (SB) carries PICH1/PICH2.
    // F=00/11 is reserved: treat both halves as control and let the CRC decide.
    std::array<HalfKind, 2> kinds{HalfKind::facch, HalfKind::facch};
    if (f.rich.f == 0b01) kinds = {HalfKind::pich, HalfKind::pich};
    else if (f.rich.f == 0b10) {
        if (f.rich.o & 0b10) kinds[0] = HalfKind::tch;   // O = 10, 11: first half TCH
        if (f.rich.o & 0b01) kinds[1] = HalfKind::tch;   // O = 01, 11: second half TCH
    }
    for (int i = 0; i < 2; ++i) {
        auto& h = f.halves[static_cast<std::size_t>(i)];
        h.kind = kinds[static_cast<std::size_t>(i)];
        h.bits = i == 0 ? f.fields.first : f.fields.second;
        if (h.kind == HalfKind::tch) {
            h.payload = traffic_payload(h.bits);
            h.llr = traffic_llr(raw, i);
        } else h.control = decode_control(h.bits);
    }
    return f;
}

Receiver::Receiver(ReceiverConfig cfg) : cfg_(cfg) {
    const double band = cfg_.pfb_channels * cfg_.spacing_hz;
    const auto decim1 = static_cast<std::size_t>(std::llround(cfg_.in_rate / band));
    if (decim1 < 1 || std::abs(static_cast<double>(decim1) * band - cfg_.in_rate) > 1e-6) throw std::invalid_argument("Input rate must be a multiple of the PFB band rate");
    resamp1_ = std::make_unique<spear::dsp::FirDecimator<spear::cf32>>("resamp1", cfg_.in_rate,
        spear::dsp::design_lowpass(cfg_.in_rate, band * 0.45, 63), decim1);
    pfb_ = std::make_unique<CenterChannelizer>(static_cast<std::size_t>(cfg_.pfb_channels), band,
        spear::dsp::design_lowpass(band, cfg_.spacing_hz / 2.0, static_cast<std::size_t>(cfg_.pfb_channels) * 8 - 1));
    const double demod_rate = cfg_.spacing_hz * cfg_.resamp2;   // 62.5 kHz
    const double sps = demod_rate / cfg_.baud;                  // 26.04
    up_ = spear::dsp::FirInterpolator<spear::cf32>("resamp2", cfg_.spacing_hz,
        spear::dsp::design_lowpass(demod_rate, cfg_.spacing_hz * 0.48, 10 * static_cast<std::size_t>(cfg_.resamp2) + 1), static_cast<std::size_t>(cfg_.resamp2));
    quad_gain_ = static_cast<float>(demod_rate / (2.0 * std::numbers::pi * cfg_.fsk_dev_hz));
    rrc_ = spear::dsp::FirDecimator<float>("rrc_rx", demod_rate, spear::std_t98::make_rx_taps(demod_rate, cfg_.baud, 0.2, static_cast<int>(sps) * 20), 1);
    sync_ = spear::std_t98::SymbolSync(sps, cfg_.sym_loop_bw, cfg_.sym_damping, cfg_.sym_ted_gain, cfg_.sym_max_dev);
    corr_ = SyncCorrelator(cfg_.sync_ratio, cfg_.sync_locked_ratio, cfg_.sync_max_misses, kFrameSymbols);
    prov_ = spear::dsp::Provenance::identity().then(resamp1_->info()).then(pfb_->info()).then(up_.info());
}

Receiver::~Receiver() = default;

void Receiver::process(std::span<const spear::cf32> in, uint64_t first_index) {
    if (!have_in0_) { in0_ = first_index; have_in0_ = true; }
    if (cfg_.freq_err_hz != 0) {
        rot_.resize(in.size());
        const double step = -2.0 * std::numbers::pi * cfg_.freq_err_hz / cfg_.in_rate;
        for (std::size_t i = 0; i < in.size(); ++i) {
            rot_[i] = in[i] * spear::cf32(static_cast<float>(std::cos(rot_phase_)), static_cast<float>(std::sin(rot_phase_)));
            rot_phase_ += step;
            if (rot_phase_ > std::numbers::pi) rot_phase_ -= 2 * std::numbers::pi;
            else if (rot_phase_ < -std::numbers::pi) rot_phase_ += 2 * std::numbers::pi;
        }
        in = rot_;
    }
    buf1_.resize(in.size() / resamp1_->decim() + 2);
    const std::size_t n1 = resamp1_->process(in, buf1_);
    for (auto& b : bins_) b.clear();
    pfb_->process(std::span<const spear::cf32>(buf1_.data(), n1), bins_);
    const auto& bin = bins_[0];
    if (bin.empty()) return;
    auto& m = metrics_;
    double power = 0;
    for (auto x : bin) power += std::norm(x);
    m.power_db = 10.0 * std::log10(power / static_cast<double>(bin.size()) + 1e-20);

    iq_up_.resize(bin.size() * static_cast<std::size_t>(cfg_.resamp2) + 2);
    const std::size_t n2 = up_.process(bin, iq_up_);
    disc_.resize(n2);
    for (std::size_t i = 0; i < n2; ++i) {
        const spear::cf32 p = iq_up_[i] * std::conj(quad_prev_);
        quad_prev_ = iq_up_[i];
        disc_[i] = std::atan2(p.imag(), p.real()) * quad_gain_;
    }
    // SPEAR: 搬送波のずれの表示は同期語の DC(下の on_candidate)から求める。原本は同期中の弁別器出力のブロック平均を
    // IIR で追っていて、真値の 6〜7 割に出ていた(2026-11-03、試験信号と実機録音で確認)
    filt_.resize(n2 + 1);
    const std::size_t n3 = rrc_.process(std::span<const float>(disc_.data(), n2), filt_);
    for (std::size_t i = 0; i < n3; ++i) filt_[i] *= cfg_.post_filt_gain;
    syms_.clear();
    sym_pos_.clear();
    const std::size_t ns = sync_.process(std::span<const float>(filt_.data(), n3), syms_, &sym_pos_);
    for (auto& s : syms_) s *= cfg_.post_sync_gain;
    m.sps = sync_.period();
    eye(n3);

    const uint64_t sym_first = symbol_index_;
    symbol_index_ += ns;
    for (std::size_t i = 0; i < ns; ++i) pos_hist_.emplace_back(sym_first + i, sym_pos_[i]);
    if (pos_hist_.size() > 1200) pos_hist_.erase(pos_hist_.begin(), pos_hist_.end() - 600);

    auto on_candidate = [&](const SyncCandidate& cand) {
        // Carrier offset appears as DC on the symbols (±2.5 ppm = ±370 Hz at 147 MHz, more than one 315 Hz level).
        // The sync word sums to zero, so its window mean is the DC; a flywheel frame reuses the last one.
        if (cand.sync_ok) {
            frame_dc_ = static_cast<float>(cand.dc);
            // SPEAR: 同期語の DC [シンボル単位] → 搬送波のずれ [Hz]。弁別器の出力は 1 = 315 Hz で、RRC(DC 利得 1)の後に
            // post_filt_gain × post_sync_gain(= 1.15)が掛かっている。シンボルの判定点(±1 / ±3)はパルスの山の高さで合わせてあるので、
            // DC はこの利得で割り戻す(試験信号で −600〜+420 Hz のずれがすべて 1.15 倍に出たことから)
            const double offset = cand.dc / (cfg_.post_filt_gain * cfg_.post_sync_gain) * cfg_.fsk_dev_hz;
            m.freq_offset_hz = m.have_offset ? 0.8 * m.freq_offset_hz + 0.2 * offset : offset;
            m.have_offset = true;
        }
        std::vector<float> corrected(cand.symbols.begin(), cand.symbols.end());
        for (auto& s : corrected) s -= frame_dc_;
        Frame f = decode_frame(corrected);
        f.symbol_index = cand.first_symbol_index;
        f.sync_sse = cand.sse;
        f.sync_ok = cand.sync_ok;
        double ch_sample = static_cast<double>(f.symbol_index) * (rrc_.info().in_rate / cfg_.baud);
        for (const auto& [index, position] : pos_hist_) if (index == f.symbol_index) { ch_sample = position; break; }
        f.input_sample_index = input_index(ch_sample);
        if (score(f) < (f.sync_burst() || f.rich.f == 0b10 ? 3 : 1)) {
            // A CRC failed: the timing loop may not have settled. Decode again at the best sample phase.
            Frame retry;
            if (decode_at(ch_sample, retry) && score(retry) > score(f)) {
                retry.symbol_index = f.symbol_index;
                retry.sync_ok = f.sync_ok;
                retry.input_sample_index = f.input_sample_index;
                f = std::move(retry);
            }
        }
        const bool crc = f.crc_ok() && f.plausible();
        pending_.push_back(std::move(f));
        return crc;
    };
    auto on_resolve = [&](uint64_t, bool emit_frame) {   // called in candidate order
        Frame f = std::move(pending_.front());
        pending_.erase(pending_.begin());
        if (emit_frame && f.plausible()) emit(f);
    };
    acq_.reserve(acq_.size() + n3);
    for (std::size_t i = 0; i < n3; ++i) acq_.push_back(filt_[i] * cfg_.post_sync_gain);
    corr_.process(syms_, sym_first, on_candidate, on_resolve);
    acquire();
    m.locked = corr_.locked();
    m.locks = corr_.stats().locks;
    m.flywheel_frames = corr_.stats().flywheel;
    m.dropped_frames = corr_.stats().dropped;
}

// std-t99-receiver-sharp: the eye tap of SPEAR apps/std_t98/receiver.cpp (filtered samples around each decision point)
void Receiver::eye(std::size_t n3) {
    if (!on_eye_) { eye_fed_ += n3; return; }
    const double sps = sync_.period();
    if (eye_buf_.empty()) eye_base_ = eye_fed_;
    eye_buf_.insert(eye_buf_.end(), filt_.begin(), filt_.begin() + static_cast<long>(n3));
    eye_pending_.insert(eye_pending_.end(), sym_pos_.begin(), sym_pos_.end());
    eye_trace_.resize(static_cast<std::size_t>(eye_points_));
    const double avail = static_cast<double>(eye_base_ + eye_buf_.size());
    std::size_t used = 0;
    for (; used < eye_pending_.size(); ++used) {
        const double start = eye_pending_[used] - 0.5 * sps;
        if (start + 2.0 * sps + 1.0 >= avail) break;
        if (start < static_cast<double>(eye_base_)) continue;
        for (int k = 0; k < eye_points_; ++k) {
            const double t = start - static_cast<double>(eye_base_) + 2.0 * sps * k / (eye_points_ - 1);
            const auto i0 = static_cast<std::size_t>(t);
            const double mu = t - static_cast<double>(i0);
            // SPEAR: 判定と同じく同期語の DC(搬送波のずれ)を引いて描く。引かないと、ずれの分だけアイが上下にずれる
            eye_trace_[static_cast<std::size_t>(k)] = (eye_buf_[i0] + static_cast<float>(mu) * (eye_buf_[i0 + 1] - eye_buf_[i0])) * cfg_.post_sync_gain - frame_dc_;
        }
        on_eye_(eye_trace_);
    }
    eye_pending_.erase(eye_pending_.begin(), eye_pending_.begin() + static_cast<long>(used));
    const double keep_from = (eye_pending_.empty() ? avail : eye_pending_.front()) - 2.0 * sps - 2.0;
    const auto drop = static_cast<std::size_t>(std::max(0.0, keep_from - static_cast<double>(eye_base_)));
    if (drop > 0 && drop <= eye_buf_.size()) { eye_buf_.erase(eye_buf_.begin(), eye_buf_.begin() + static_cast<long>(drop)); eye_base_ += drop; }
    eye_fed_ += n3;
}

uint64_t Receiver::input_index(double filtered_sample) const {
    return in0_ + static_cast<uint64_t>(std::max(0.0, prov_.input_index(filtered_sample)));
}

void Receiver::emit(const Frame& f) {
    const auto tolerance = static_cast<uint64_t>(cfg_.in_rate / cfg_.baud / 2);   // half a symbol
    for (uint64_t r : recent_)
        if ((f.input_sample_index > r ? f.input_sample_index - r : r - f.input_sample_index) < tolerance) return;
    // std-t99-receiver-sharp: a frame without any CRC must sit on the 80 ms grid of the last frame with a CRC
    // (within 2 s). Identical voice frames repeat the same symbols every frame, so a sync-word look-alike inside
    // the data can recur every 192 symbols; frames found there are not output.
    if (!f.crc_ok() && have_grid_) {
        const double frame = cfg_.in_rate * kFrameSymbols / cfg_.baud;
        const double delta = static_cast<double>(f.input_sample_index) - static_cast<double>(grid_);
        double phase = std::fmod(std::abs(delta), frame);
        phase = std::min(phase, frame - phase);
        if (std::abs(delta) < 2.0 * cfg_.in_rate && phase > static_cast<double>(tolerance)) return;
    }
    if (f.crc_ok()) { grid_ = f.input_sample_index; have_grid_ = true; }
    recent_.push_back(f.input_sample_index);
    if (recent_.size() > 16) recent_.erase(recent_.begin());
    ++metrics_.frames;
    if (f.crc_ok()) ++metrics_.crc_frames;
    if (on_frame_) on_frame_(f);
}

int Receiver::score(const Frame& f) {
    int n = f.sacch.crc_ok;
    for (const auto& h : f.halves) n += h.control && h.control->crc_ok;
    return n;
}

double Receiver::sync_metric(double p, float& dc) const {
    const double sps = rrc_.info().in_rate / cfg_.baud;
    std::array<float, 10> v;
    dc = 0;
    for (std::size_t k = 0; k < v.size(); ++k) { v[k] = acq_at(p + static_cast<double>(k) * sps); dc += v[k]; }
    dc /= static_cast<float>(v.size());
    double sse = 0;
    for (std::size_t k = 0; k < v.size(); ++k) { const double d = v[k] - dc - kSyncWord[k]; sse += d * d; }
    return sse;
}

float Receiver::acq_at(double x) const {
    const double r = x - static_cast<double>(acq_base_);
    const auto i = static_cast<std::size_t>(r);
    const float mu = static_cast<float>(r - static_cast<double>(i));
    return acq_[i] + mu * (acq_[i + 1] - acq_[i]);
}

bool Receiver::decode_at(double position, Frame& f, double* best_position) const {
    const double sps = rrc_.info().in_rate / cfg_.baud;
    const double first = position - sps / 2, last = position + sps / 2;
    if (first < static_cast<double>(acq_base_) || last + (kFrameSymbols + 1) * sps + 2 >= static_cast<double>(acq_base_ + acq_.size())) return false;
    // Best phase within one symbol (quarter-sample steps) by the DC-free sync metric
    double best_p = first, best = 1e30;
    float dc = 0;
    for (double q = first; q <= last; q += 0.25) {
        const double v = sync_metric(q, dc);
        if (v < best) { best = v; best_p = q; }
    }
    sync_metric(best_p, dc);
    std::vector<float> raw(kFrameSymbols);
    for (int k = 0; k < kFrameSymbols; ++k) raw[static_cast<std::size_t>(k)] = acq_at(best_p + k * sps) - dc;
    f = decode_frame(raw);
    f.sync_sse = best;
    f.sync_ok = true;
    f.input_sample_index = input_index(best_p);
    if (best_position) *best_position = best_p;
    return true;
}

void Receiver::acquire() {
    const double sps = rrc_.info().in_rate / cfg_.baud;
    const double threshold = cfg_.sync_ratio * 74.0;   // same strict ratio as SEARCH (sync word energy 74)
    const double end = static_cast<double>(acq_base_ + acq_.size());
    if (acq_next_ < static_cast<double>(acq_base_) + sps) acq_next_ = static_cast<double>(acq_base_) + sps;
    const double frame_span = (kFrameSymbols + 2) * sps + 2;
    while (acq_next_ + frame_span < end) {
        float dc = 0;
        if (sync_metric(acq_next_, dc) > threshold) {
            acq_next_ += 1;
            if (acq_follow_ > 0 && acq_next_ > acq_follow_ + sps) acq_follow_ = -1;
            continue;
        }
        Frame f;
        double where = acq_next_;
        decode_at(acq_next_ + sps / 2, f, &where);   // the best phase lies within the next symbol
        bool strong = false;
        for (const auto& h : f.halves) strong |= h.control && h.control->crc_ok;
        // std-t99-receiver-sharp: exactly one frame after an acquired frame, the sync word, a valid RICH and the
        // SACCH CRC are enough. This keeps the voice frame after a synchronous burst (no CRC-12) while the symbol
        // path is still pulling in.
        const bool follows = acq_follow_ > 0 && std::abs(where - acq_follow_) <= sps && f.sacch.crc_ok && f.plausible();
        // Following continues while the symbol path is locked: emit() drops the duplicates when both paths agree,
        // and keeps the transmission when the symbol path has locked onto a look-alike (see emit()).
        if ((strong && (!corr_.locked() || f.sync_burst())) || follows) {
            emit(f);
            acq_next_ = where + (kFrameSymbols - 1) * sps;   // the next frame starts one frame later
            acq_follow_ = where + kFrameSymbols * sps;
        } else {
            acq_next_ = where + sps / 2;
            if (acq_follow_ > 0 && acq_next_ > acq_follow_ + sps) acq_follow_ = -1;
        }
    }
    // Keep two frames of history for re-decoding symbol-path candidates
    const auto keep_from = static_cast<uint64_t>(std::max(0.0, acq_next_ - 2 * frame_span));
    if (keep_from > acq_base_) {
        const auto drop = std::min<uint64_t>(keep_from - acq_base_, acq_.size());
        acq_.erase(acq_.begin(), acq_.begin() + static_cast<long>(drop));
        acq_base_ += drop;
    }
}

} // namespace spear::std_t99
