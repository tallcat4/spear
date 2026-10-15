#include "receiver.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace spear::jr_train {

namespace {
// DSP の遅延の推定値 [シンボル]: AGC の窓 64 sample(19.2 kS/s)= 16、整合フィルタの窓 ≒ 10、MLSE のトレースバック 32。
// フレームの radio.rx index は概算にとどまる(実録音で較正する。docs/apps/jr_train.md「未検証」)
constexpr double kDspLatencySymbols = 58;
}

struct Receiver::Channel {
    int index = 0;
    double offset_hz = 0;
    std::unique_ptr<dsp::Demodulator> demod;
    protocol::Synchronizer sync;
    std::vector<uint8_t> dibits;
    std::vector<protocol::Frame> frames;
    ChannelStatus status;
    uint64_t epoch_work_start = 0;   // この epoch の DSP が最初に受けた work_rate の通し番号
    std::size_t worker = 0;
    bool failed = false;

    void reset(double rate) {
        demod = std::make_unique<dsp::Demodulator>(rate, offset_hz);
        sync = protocol::Synchronizer{};
        dibits.clear();
        frames.clear();
    }
};

Receiver::Receiver(ReceiverConfig cfg) : cfg_(std::move(cfg)) {
    if (!(cfg_.in_rate > 0)) throw std::invalid_argument("in_rate must be positive");
    decim_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(cfg_.in_rate / cfg_.work_rate_target)));
    work_rate_ = cfg_.in_rate / static_cast<double>(decim_);
    prov_ = spear::dsp::Provenance::identity();
    const double angle = -2.0 * std::numbers::pi * cfg_.rotate_hz / cfg_.in_rate;
    step_re_ = std::cos(angle);
    step_im_ = std::sin(angle);
    if (decim_ > 1) {
        // 帯域は ±68.6 kHz(端のチャネル ±65.625 kHz + 占有帯域の半分 2.9 kHz)。通過域を 85 kHz まで取り、
        // work_rate − 68.6 kHz より上を十分落とす(Hamming、遷移幅 ≒ 3.3 × in_rate / taps)
        const std::size_t taps = 16 * decim_ + 1;
        const double cutoff = std::min(85e3, 0.36 * work_rate_);
        front_ = std::make_unique<spear::dsp::FirDecimator<cf32>>("jr_train.front", cfg_.in_rate, spear::dsp::design_lowpass(cfg_.in_rate, cutoff, taps), decim_);
        prov_ = prov_.then(front_->info());
    }
    for (std::size_t i = 0; i < cfg_.channel_offsets_hz.size(); ++i) {
        auto c = std::make_unique<Channel>();
        c->index = static_cast<int>(i);
        c->offset_hz = cfg_.channel_offsets_hz[i];
        if (auto e = dsp::validate(work_rate_, c->offset_hz); !e.empty()) throw std::invalid_argument("channel " + std::to_string(i) + ": " + e);
        c->reset(work_rate_);
        ch_.push_back(std::move(c));
    }
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());   // 0(不明)でも 1 以上
    std::size_t n = cfg_.threads > 0 ? static_cast<std::size_t>(cfg_.threads) : hw - 1;
    n = std::clamp<std::size_t>(n, 1, std::max<std::size_t>(1, ch_.size()));
    for (auto& c : ch_) c->worker = static_cast<std::size_t>(c->index) % n;
    if (n > 1)
        for (std::size_t w = 0; w < n; ++w) workers_.emplace_back([this, w] { work(w); });
}

Receiver::~Receiver() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_start_.notify_all();
    for (auto& t : workers_) t.join();
}

void Receiver::run_channel(Channel& c) {
    c.dibits.clear();
    c.frames.clear();
    if (c.failed) return;
    try {
        c.demod->process(work_in_, c.dibits);
    } catch (const std::exception&) {
        c.failed = true;   // 呼び出し元のスレッドで作り直す
        return;
    }
    for (uint8_t d : c.dibits) c.sync.push(d, c.frames);
}

void Receiver::work(std::size_t worker) {
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_start_.wait(lk, [&] { return quit_ || round_ != seen; });
            if (quit_) return;
            seen = round_;
        }
        for (auto& c : ch_)
            if (c->worker == worker) run_channel(*c);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (--pending_ == 0) cv_done_.notify_one();
        }
    }
}

void Receiver::process(std::span<const cf32> in, uint64_t first_index) {
    if (!have_in0_) {
        in0_ = first_index;
        have_in0_ = true;
    }
    // 1) 回転: 帯域中心を DC へ。複素 NCO(double)の漸化式、4096 sample ごとに振幅を正規化(ブロック境界でも位相は連続)
    if (cfg_.rotate_hz != 0) {
        rot_.resize(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            const double re = in[i].real(), im = in[i].imag();
            rot_[i] = cf32(static_cast<float>(re * osc_re_ - im * osc_im_), static_cast<float>(re * osc_im_ + im * osc_re_));
            const double next = osc_re_ * step_re_ - osc_im_ * step_im_;
            osc_im_ = osc_re_ * step_im_ + osc_im_ * step_re_;
            osc_re_ = next;
            if (++renorm_ == 4096) {
                const double m = std::sqrt(osc_re_ * osc_re_ + osc_im_ * osc_im_);
                osc_re_ /= m;
                osc_im_ /= m;
                renorm_ = 0;
            }
        }
        in = rot_;
    }
    // 2) 前段の間引き
    std::span<const cf32> work = in;
    if (front_) {
        work_buf_.resize(in.size() / decim_ + 2);
        const std::size_t n = front_->process(in, work_buf_);
        work = std::span<const cf32>(work_buf_.data(), n);
    }
    const uint64_t work_first = work_index_;
    work_index_ += work.size();
    if (obs_.band && !work.empty()) obs_.band(work, work_first);
    if (work.empty()) return;
    // 3) 全チャネル(worker で並列)
    work_in_ = work;
    if (workers_.empty()) {
        for (auto& c : ch_) run_channel(*c);
    } else {
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_ = workers_.size();
            ++round_;
        }
        cv_start_.notify_all();
        std::unique_lock<std::mutex> lk(mu_);
        cv_done_.wait(lk, [&] { return pending_ == 0; });
    }
    work_in_ = {};
    // 4) 状態とフレーム(呼び出し元のスレッド)
    const double work_per_symbol = work_rate_ / dsp::kSymbolRate;
    for (auto& cp : ch_) {
        Channel& c = *cp;
        if (c.failed) {
            c.failed = false;
            c.reset(work_rate_);
            c.epoch_work_start = work_index_;
            ++c.status.epoch;
            ++c.status.resets;
            c.status.frame_locked = false;
            continue;
        }
        c.status.snap = c.demod->snapshot();
        c.status.frame_locked = c.sync.locked;
        c.status.frames = c.sync.frames;
        c.status.synchronized_frames = c.sync.synchronized_frames;
        for (const auto& f : c.frames) {
            Frame out;
            out.channel = c.index;
            out.epoch = c.status.epoch;
            out.bit_offset = f.bit_offset;
            out.sync_matched = f.sync_matched;
            out.marker_errors = f.marker_errors;
            out.received = f.received;
            const double work_sample = static_cast<double>(c.epoch_work_start) + (static_cast<double>(f.bit_offset / 2) - kDspLatencySymbols) * work_per_symbol;
            out.input_sample_index = in0_ + static_cast<uint64_t>(std::max(0.0, prov_.input_index(std::max(0.0, work_sample))));
            if (obs_.frame) obs_.frame(out);
        }
    }
}

void Receiver::discontinuity() {
    for (auto& c : ch_) {
        c->reset(work_rate_);
        c->failed = false;
        c->epoch_work_start = work_index_;
        ++c->status.epoch;
        ++c->status.resets;
        c->status.frame_locked = false;
        c->status.evm_percent = 0;
    }
}

const ChannelStatus& Receiver::status(int ch) const { return ch_.at(static_cast<std::size_t>(ch))->status; }

double Receiver::read_signal(int ch, std::vector<std::complex<float>>& points, std::size_t capacity) {
    auto& c = *ch_.at(static_cast<std::size_t>(ch));
    c.status.evm_percent = c.demod->read_signal(points, capacity);
    return c.status.evm_percent;
}

void Receiver::update_signal() {
    std::vector<std::complex<float>> tmp;
    for (int i = 0; i < num_channels(); ++i) read_signal(i, tmp);
}

} // namespace spear::jr_train
