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
    StreamMeta tm = m;
    tm.id = kRadioTxStreamId;
    tx_ = std::make_unique<TxStream>(tm, events_, tx_arm_);
}

ThreadedSource::~ThreadedSource() { stop(); }

bool ThreadedSource::configure(const RfConfig& cfg, std::string* err) {
    if (running_) { if (err) *err = "cannot configure while running"; return false; }
    if (!check_direction(cfg, err)) return false;
    {
        std::lock_guard lk(cfg_mu_);
        cfg_ = cfg;
    }
    bump_state();
    {
        StreamMeta tm = tx_->meta();
        tm.sample_rate = cfg.sample_rate;
        tm.center_freq = cfg.center_freq;
        tm.bandwidth   = cfg.bandwidth > 0 ? cfg.bandwidth : cfg.sample_rate;
        tx_ = std::make_unique<TxStream>(tm, events_, tx_arm_);   // 前の App が閉じたキューは使い回さない
    }
    // stream metadata は生成時に確定 (§3.2)。source の再構成 = stream の作り直し。
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
    if (config().direction == Direction::TX) { th_ = std::thread([this] { run_tx(); }); return; }
    on_start();
    th_ = std::thread([this] { run(); });
}

void ThreadedSource::stop() {
    // EOS で thread が自然終了した後も join が必要(joinable な thread の破棄は terminate)
    stop_req_ = true;
    tx_->close();   // 送信中の App の write() を解く(Core::stop_app は Source を先に止めてから App を止める)
    if (th_.joinable()) th_.join();
    if (running_.exchange(false)) { if (config().direction != Direction::TX) on_stop(); state_since_ns_ = host_now_ns(); }
}

bool ThreadedSource::set_tx_gain(double db) {
    {
        std::lock_guard lk(cfg_mu_);
        cfg_.tx_gain = db;
    }
    bump_state();
    if (events_) events_->emit(EventKind::TxGain, name_, "tx gain set (no hardware)", {}, std::llround(db));
    return true;
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

// 送信(装置なし): 装置と同じく sample rate で消費する。burst の途中でキューが空のまま消費の期限を過ぎたら、
// 装置なら FIFO が空になる状況なので Underflow を報告する(§4.3 の TX 不変条件を装置なしでも同じ経路で確かめる)
void ThreadedSource::run_tx() {
    using clock = std::chrono::steady_clock;
    const double rate = config().sample_rate > 0 ? config().sample_rate : 1e6;
    const auto slack = std::chrono::milliseconds(5);
    auto due = clock::now();
    bool in_burst = false, late = false;
    uint64_t index = 0;   // burst をまたいだ送信済み sample 数(event の範囲)
    while (!stop_req_) {
        auto ob = tx_->pop(std::chrono::milliseconds(10));
        const auto now = clock::now();
        if (in_burst && !tx_->armed()) {   // TX MASTER が SAFE になった: burst を直ちに閉じる
            in_burst = false;
            tx_->note_burst(false);
            if (events_) events_->emit(EventKind::TxBurst, name_, "burst end (TX MASTER SAFE)", {generation_.load(), index, index}, 0);
            continue;
        }
        if (!ob) {
            if (tx_->closed()) break;
            if (in_burst && realtime_ && !late && now > due + slack) {
                late = true;
                tx_->note_underflow(1, {generation_.load(), index, index}, name_, "tx queue empty mid-burst (producer late)");
            }
            continue;
        }
        const auto& h = ob->header();
        if (!in_burst && (h.flags.has(Flag::StartOfBurst) || h.sample_count > 0)) {
            in_burst = true; late = false; due = now;
            tx_->note_burst(true);
        }
        if (h.sample_count > 0) {
            if (late) { late = false; due = now; }   // 装置は空の間 0 を出していた。時間軸はここから
            tx_sink_->write(Delivery{*ob, h.flags});
            tx_->note_sent(*ob);
            index += h.sample_count;
            sample_index_pub_.store(index);
            if (realtime_) {
                due += std::chrono::nanoseconds(static_cast<int64_t>(1e9 * h.sample_count / rate));
                std::this_thread::sleep_until(due);
            }
        }
        if (h.flags.has(Flag::EndOfBurst)) { in_burst = false; tx_->note_burst(false); }
    }
    if (in_burst) tx_->note_burst(false);
    tx_sink_->close();
}

} // namespace spear
