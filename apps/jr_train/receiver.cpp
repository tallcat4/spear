#include "receiver.hpp"

#include <algorithm>
#include <chrono>
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
    std::unique_ptr<Pipeline> pipe;
    // worker が 1 ブロックで作ったもの(呼び出し元のスレッドで Observer に渡す)
    std::vector<FrameEvent> events;
    std::vector<Audio> audio;              // 音声合成のタスクが作ったもの(前のブロックの分)
    std::vector<VoiceOp> voice_ops;        // このブロックで実行する合成への操作(前のブロックで溜めたもの)
    int64_t cost_ns = 0;                   // 直前のブロックの DSP の処理時間(取る順を決める)
    bool failed = false, voice_failed = false;
    uint64_t resets = 0;
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
    for (std::size_t i = 0; i < cfg_.channels.size(); ++i) {
        const auto& cc = cfg_.channels[i];
        if (auto e = dsp::validate(work_rate_, cc.frequency_hz - cfg_.band_center_hz); !e.empty()) throw std::invalid_argument(cc.label + ": " + e);
        auto c = std::make_unique<Channel>();
        c->index = static_cast<int>(i);
        c->pipe = std::make_unique<Pipeline>(cc, work_rate_, cfg_.band_center_hz);
        c->pipe->set_deferred_voice(true);
        ch_.push_back(std::move(c));
    }
    // 既定は物理コア数(SMT の兄弟スレッドまで使うと、電池駆動では全コアのクロックが下がって遅くなる。docs/apps/jr_train.md「負荷と最適化」)
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());   // 0(不明)でも 1 以上
    std::size_t n = cfg_.threads > 0 ? static_cast<std::size_t>(cfg_.threads) : std::max(1u, hw / 2);
    n = std::clamp<std::size_t>(n, 1, std::max<std::size_t>(1, ch_.size()));
    for (std::size_t w = 1; w < n; ++w) workers_.emplace_back([this] { work(); });
}

Receiver::~Receiver() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_start_.notify_all();
    for (auto& t : workers_) t.join();
}

// 同じチャネルの 2 つのタスクは同時に走るが、触る状態は分かれている(DSP: 合成器以外、音声: 合成器と audio_frames)。
// 失敗は呼び出し元のスレッドで作り直す(原本は受信全体を止める。ここではそのチャネルだけやり直す)
void Receiver::run_task(const Task& t) {
    Channel& c = *t.channel;
    if (t.voice) {
        try {
            c.audio = c.pipe->run_voice(c.voice_ops);
        } catch (const std::exception&) {
            c.voice_failed = true;
        }
        return;
    }
    c.events.clear();
    if (c.failed) return;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        c.pipe->process(work_in_, [&](const FrameEvent& ev) { c.events.push_back(ev); });
    } catch (const std::exception&) {
        c.failed = true;
    }
    c.cost_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

// 残っているタスクを 1 つずつ取って処理する(呼び出し元と worker の両方が回す)
void Receiver::run_round() {
    for (std::size_t i; (i = next_.fetch_add(1, std::memory_order_relaxed)) < tasks_.size();) run_task(tasks_[i]);
}

// 溜まっている合成への操作を今すぐ実行する(区間の切れ目と入力の終わり)。deliver なら音声を Observer に渡す
void Receiver::finish_voice(bool deliver) {
    for (auto& c : ch_) {
        std::vector<Audio> audio;
        try {
            audio = c->pipe->run_voice(c->pipe->take_voice_ops());
        } catch (const std::exception&) {
            continue;   // このあと作り直す(discontinuity)か、入力の終わり
        }
        if (deliver && obs_.audio)
            for (const auto& a : audio) obs_.audio(c->index, a);
    }
}

void Receiver::work() {
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_start_.wait(lk, [&] { return quit_ || round_ != seen; });
            if (quit_) return;
            seen = round_;
        }
        run_round();
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
    // タスク: 前のブロックで溜めた音声合成(重いので先に)と、このブロックの DSP(前のブロックで重かった順)
    tasks_.clear();
    for (auto& c : ch_) {
        c->audio.clear();
        c->voice_failed = false;
        c->voice_ops = c->pipe->take_voice_ops();
        if (!c->voice_ops.empty()) tasks_.push_back({c.get(), true});
    }
    const auto pushes = [](const Channel* c) {
        return std::count_if(c->voice_ops.begin(), c->voice_ops.end(), [](const VoiceOp& op) { return op.kind == VoiceOp::Kind::Push; });
    };
    std::stable_sort(tasks_.begin(), tasks_.end(), [&](const Task& a, const Task& b) { return pushes(a.channel) > pushes(b.channel); });
    const std::size_t voice_tasks = tasks_.size();
    for (auto& c : ch_) tasks_.push_back({c.get(), false});
    std::stable_sort(tasks_.begin() + static_cast<std::ptrdiff_t>(voice_tasks), tasks_.end(),
                     [](const Task& a, const Task& b) { return a.channel->cost_ns > b.channel->cost_ns; });
    // 出力は取る順に依らない(各タスクは独立で、Observer は下でチャネルの順に呼ぶ)
    if (workers_.empty()) {
        for (const auto& t : tasks_) run_task(t);
    } else {
        next_.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_ = workers_.size();
            ++round_;
        }
        cv_start_.notify_all();
        run_round();
        std::unique_lock<std::mutex> lk(mu_);
        cv_done_.wait(lk, [&] { return pending_ == 0; });
    }
    work_in_ = {};
    // 4) フレームと音声(呼び出し元のスレッド、チャネルの順)
    const double work_per_symbol = work_rate_ / dsp::kSymbolRate;
    const auto to_us = [&](uint64_t work_samples) { return static_cast<uint64_t>(static_cast<double>(work_samples) / work_rate_ * 1e6); };
    for (auto& cp : ch_) {
        Channel& c = *cp;
        if (obs_.audio && !c.voice_failed)   // 前のブロックの音声(このブロックの DSP が失敗しても前のブロックの分は有効)
            for (const auto& a : c.audio) obs_.audio(c.index, a);
        if (c.failed || c.voice_failed) {
            c.failed = false;
            c.voice_failed = false;
            c.pipe->discontinuity();   // このブロックで溜めた合成への操作も捨てる
            ++c.resets;
            c.events.clear();
            c.audio.clear();
            continue;
        }
        for (const auto& ev : c.events) {
            FrameTiming t;
            const double work_sample = static_cast<double>(segment_start_) + (static_cast<double>(ev.frame.bit_offset / 2) - kDspLatencySymbols) * work_per_symbol;
            t.input_sample_index = in0_ + static_cast<uint64_t>(std::max(0.0, prov_.input_index(std::max(0.0, work_sample))));
            t.time_us = to_us(segment_start_) + ev.frame.bit_offset * 1'000'000 / 9600;
            if (obs_.frame) obs_.frame(c.index, ev, t);
        }
        c.events.clear();
        c.audio.clear();
    }
}

void Receiver::discontinuity() {
    finish_voice(true);   // 切れ目の前のブロックの音声(同期なら前の process() で渡っていた分)
    for (auto& c : ch_) {
        c->pipe->discontinuity();
        c->failed = false;
    }
    segment_start_ = work_index_;
}

void Receiver::flush() {
    finish_voice(true);
    for (auto& c : ch_) {
        std::vector<Audio> tail;
        try {
            c->pipe->flush();   // 合成への操作(先読みの吐き出し)を溜める
            tail = c->pipe->run_voice(c->pipe->take_voice_ops());
        } catch (const std::exception&) {
            continue;
        }
        if (obs_.audio)
            for (const auto& a : tail) obs_.audio(c->index, a);
    }
}

const PipelineStatus& Receiver::pipeline_status(int ch) const { return ch_.at(static_cast<std::size_t>(ch))->pipe->status(); }
const PipelineStatus& Receiver::snapshot(int ch) { return ch_.at(static_cast<std::size_t>(ch))->pipe->snapshot(); }
std::vector<protocol::Message> Receiver::take_messages(int ch) { return ch_.at(static_cast<std::size_t>(ch))->pipe->take_messages(); }
uint64_t Receiver::resets(int ch) const { return ch_.at(static_cast<std::size_t>(ch))->resets; }
uint64_t Receiver::time_us() const { return static_cast<uint64_t>(static_cast<double>(work_index_) / work_rate_ * 1e6); }

} // namespace spear::jr_train
