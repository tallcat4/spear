#include "spear/core/source.hpp"

#include <chrono>
#include <cmath>
#include <thread>

namespace spear {

ThreadedSource::ThreadedSource(std::string name, EventBus* events, std::size_t block_samples,
                               std::size_t pool_blocks)
    : name_(std::move(name)), events_(events), block_samples_(block_samples),
      pool_(block_samples * sizeof(sc16), pool_blocks) {
    StreamMeta m;
    m.id = kRadioRxStreamId;
    m.dtype = DataType::ComplexInt16;
    m.unit = "FS";
    m.direction = Direction::RX;
    stream_ = std::make_unique<Stream<sc16>>(m, events_);
}

ThreadedSource::~ThreadedSource() { stop(); }

bool ThreadedSource::configure(const RfConfig& cfg, std::string* err) {
    if (running_) { if (err) *err = "cannot configure while running"; return false; }
    {
        std::lock_guard lk(cfg_mu_);
        cfg_ = cfg;
    }
    bump_state();
    // stream metadata は生成時に確定 (§4.2)。source の再構成 = stream の作り直し。
    StreamMeta m = stream_->meta();
    m.sample_rate = cfg.sample_rate;
    m.center_freq = cfg.center_freq;
    m.bandwidth   = cfg.bandwidth > 0 ? cfg.bandwidth : cfg.sample_rate;
    if (stream_->consumer_count() == 0) stream_ = std::make_unique<Stream<sc16>>(m, events_);
    else if (err) *err = "consumers attached; metadata not updated";
    return true;
}

void ThreadedSource::start() {
    if (running_.exchange(true)) return;
    state_since_ns_ = host_now_ns();
    stop_req_ = false;
    sequence_ = 0;
    sample_index_ = 0;
    on_start();
    th_ = std::thread([this] { run(); });
}

void ThreadedSource::stop() {
    // EOS で thread が自然終了した後も join が必要(joinable な thread の破棄は terminate)
    stop_req_ = true;
    if (th_.joinable()) th_.join();
    if (running_.exchange(false)) { on_stop(); state_since_ns_ = host_now_ns(); }
}

bool ThreadedSource::retune(double center_freq_hz) {
    {
        std::lock_guard lk(cfg_mu_);
        cfg_.center_freq = center_freq_hz;
    }
    bump_state();
    if (events_) {
        const uint64_t idx = sample_index_pub_.load();
        events_->emit(EventKind::Retune, name_, "center frequency changed", {generation_.load(), idx, idx},
                      static_cast<int64_t>(std::llround(center_freq_hz)));
    }
    return true;
}

void ThreadedSource::next_generation() {
    ++generation_;
    sample_index_ = 0;
    if (events_) events_->emit(EventKind::ClockReset, name_, "generation advanced", {}, static_cast<int64_t>(generation_.load()));
}

void ThreadedSource::run() {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    uint64_t paced = 0;                       // 実時間ペーシング用の累計 sample 数
    const double rate = config().sample_rate > 0 ? config().sample_rate : 1e6;
    bool first = true;

    while (!stop_req_) {
        auto b = pool_.acquire<sc16>(block_samples_);
        const uint64_t gen_before = generation_.load();
        const uint32_t n = fill(b);
        if (n == 0) {
            // EOS: 空 block に EndOfStream を立てて配送し、stream を閉じる
            b.header().flags.set(Flag::EndOfStream);
            b.header().generation = generation_;
            b.header().sequence = sequence_++;
            b.header().sample_index = sample_index_;
            b.header().host_ns = host_now_ns();
            stream_->publish(b.commit(0));
            stream_->end();
            break;
        }
        auto& h = b.header();
        if (h.generation != gen_before) { /* fill() が generation を進めた: sample_index は 0 から */ }
        h.generation = generation_;
        h.sequence = sequence_++;
        h.sample_index = sample_index_;
        h.host_ns = host_now_ns();
        // 合成/再生には hardware clock が無い。sample index から導出した時刻を "hw_time" とする。
        const double t = static_cast<double>(sample_index_) / rate;
        h.hw_time.full_secs = static_cast<int64_t>(t);
        h.hw_time.frac_secs = t - static_cast<double>(h.hw_time.full_secs);
        h.hw_time.valid = true;
        if (first) { h.flags.set(Flag::StartOfBurst); first = false; }
        sample_index_ += n;
        sample_index_pub_.store(sample_index_);
        stream_->publish(b.commit(n));

        if (realtime_) {
            paced += n;
            const auto due = t0 + std::chrono::nanoseconds(static_cast<int64_t>(1e9 * static_cast<double>(paced) / rate));
            std::this_thread::sleep_until(due);
        }
    }
    running_ = false;
}

} // namespace spear
