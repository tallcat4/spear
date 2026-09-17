#include "spear/core/recording.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace spear {

// ---- RecordingSource -----------------------------------------------------------

RecordingSource::RecordingSource(EventBus* events, std::string base_path, bool loop, std::size_t block_samples)
    : ThreadedSource("recording", events, block_samples), base_(std::move(base_path)), loop_(loop) {}

bool RecordingSource::open(std::string* err) {
    std::string e;
    if (!sigmf::read(base_, meta_, &e)) {
        if (err) *err = e;
        if (events_) events_->emit(EventKind::Error, name_, "open failed: " + e);
        return false;
    }
    if (meta_.dtype != DataType::ComplexInt16 && meta_.dtype != DataType::ComplexFloat32) {
        if (err) *err = "unsupported datatype";
        return false;
    }
    RfConfig c;
    c.sample_rate = meta_.sample_rate;
    c.center_freq = meta_.captures.empty() ? 0.0 : meta_.captures.front().frequency;
    opened_ = true;
    return ThreadedSource::configure(c, err);
}

bool RecordingSource::configure(const RfConfig& cfg, std::string* err) {
    if (!opened_) return ThreadedSource::configure(cfg, err);
    RfConfig c = config();
    if (cfg.sample_rate != c.sample_rate || cfg.center_freq != c.center_freq) {
        char msg[200];
        std::snprintf(msg, sizeof msg, "recording is fixed at %.0f sps / %.6f MHz; requested %.0f sps / %.6f MHz ignored",
                      c.sample_rate, c.center_freq / 1e6, cfg.sample_rate, cfg.center_freq / 1e6);
        if (events_) events_->emit(EventKind::Warning, name_, msg);
    }
    c.gain = cfg.gain; c.agc = cfg.agc; c.antenna = cfg.antenna;   // 無意味だが害もない(表示用)
    return ThreadedSource::configure(c, err);
}

bool RecordingSource::retune(double center_freq_hz) {
    char msg[160];
    std::snprintf(msg, sizeof msg, "recording center is fixed at %.6f MHz; retune to %.6f MHz ignored", config().center_freq / 1e6, center_freq_hz / 1e6);
    if (events_) events_->emit(EventKind::Warning, name_, msg);
    return false;
}

bool RecordingSource::reopen_data() {
    data_.close();
    data_.clear();
    data_.open(sigmf::data_path(base_), std::ios::binary);
    file_pos_samples_ = 0;
    capture_idx_ = 0;
    return static_cast<bool>(data_);
}

void RecordingSource::on_start() {
    if (!reopen_data() && events_) events_->emit(EventKind::Error, name_, "cannot open " + sigmf::data_path(base_));
}

void RecordingSource::on_stop() { data_.close(); }

uint32_t RecordingSource::fill(BlockBuilder& b) {
    if (!data_) return 0;
    auto out = b.data<sc16>();
    const std::size_t esz = dtype_size(meta_.dtype);
    // capture 境界(retune)を block 境界に揃え、Retune flag を立てる
    std::size_t want = out.size();
    if (capture_idx_ + 1 < meta_.captures.size()) {
        const uint64_t next = meta_.captures[capture_idx_ + 1].sample_start;
        if (file_pos_samples_ < next) want = std::min<std::size_t>(want, next - file_pos_samples_);
    }
    if (capture_idx_ < meta_.captures.size() && file_pos_samples_ == meta_.captures[capture_idx_].sample_start) {
        if (capture_idx_ > 0) b.header().flags.set(Flag::Retune);
        ++capture_idx_;
    }
    tmp_.resize(want * esz);
    data_.read(reinterpret_cast<char*>(tmp_.data()), static_cast<std::streamsize>(tmp_.size()));
    std::size_t got = static_cast<std::size_t>(data_.gcount()) / esz;
    if (got == 0) {
        if (!loop_) return 0;
        // loop: 時間軸が巻き戻る → generation を進める (§4.4.1)
        if (!reopen_data()) return 0;
        next_generation();
        b.header().flags.set(Flag::Discontinuity);
        data_.read(reinterpret_cast<char*>(tmp_.data()), static_cast<std::streamsize>(tmp_.size()));
        got = static_cast<std::size_t>(data_.gcount()) / esz;
        if (got == 0) return 0;
    }
    if (meta_.dtype == DataType::ComplexInt16) {
        std::memcpy(out.data(), tmp_.data(), got * sizeof(sc16));
    } else {
        const auto* src = reinterpret_cast<const cf32*>(tmp_.data());
        for (std::size_t i = 0; i < got; ++i) {
            auto q = [](float v) { return static_cast<int16_t>(std::lrint(std::clamp(v, -1.f, 1.f) * 32767.f)); };
            out[i] = sc16(q(src[i].real()), q(src[i].imag()));
        }
    }
    file_pos_samples_ += got;
    return static_cast<uint32_t>(got);
}

// ---- SigmfRecorder ---------------------------------------------------------------

SigmfRecorder::SigmfRecorder(std::string base_path, const StreamMeta& sm, const RfConfig& cfg, EventBus* events)
    : base_(std::move(base_path)), events_(events) {
    meta_.dtype = sm.dtype;
    meta_.sample_rate = sm.sample_rate;
    meta_.description = "spear recording, stream=" + sm.id;
    initial_freq_ = cfg.center_freq;
    data_.open(sigmf::data_path(base_), std::ios::binary | std::ios::trunc);
    ok_ = static_cast<bool>(data_);
    if (!ok_ && events_) events_->emit(EventKind::Error, name(), "cannot open " + sigmf::data_path(base_));
    if (events_) listener_ = events_->subscribe([this](const Event& e) { on_event(e); });
}

SigmfRecorder::~SigmfRecorder() { close(); }

void SigmfRecorder::on_event(const Event& e) {
    // 記録中の全 event を sidecar へ(録音後の再解析で「その時何が起きていたか」が分かる)
    std::lock_guard lk(mu_);
    if (closed_) return;
    meta_.events.push_back({e.host_ns, std::string(to_string(e.kind)), e.source, e.detail, e.range, e.value});
    // Retune event: value = 新周波数 [Hz], range.begin = その周波数が始まる sample index
    if (e.kind == EventKind::Retune)
        meta_.tuning.push_back({e.range.generation, e.range.begin, static_cast<double>(e.value), e.host_ns});
    // TimeReference event: detail "hw=<s> s mono=<ns> ns utc=<ns> ns (...)", value = utc_ns
    if (e.kind == EventKind::TimeReference) {
        sigmf::TimeRef t;
        t.generation = e.range.generation;
        t.hw_sample_index = e.range.begin;
        t.utc_ns = static_cast<uint64_t>(e.value);
        std::sscanf(e.detail.c_str(), "hw=%lf s mono=%llu", &t.hw_time_s, (unsigned long long*)&t.host_mono_ns);
        meta_.time_refs.push_back(t);
    }
}

void SigmfRecorder::write(const Delivery& d) {
    std::lock_guard lk(mu_);
    if (!ok_ || closed_) return;
    const auto& h = d.block.header();
    if (h.sample_count == 0 && d.flags.has(Flag::EndOfStream)) return;

    const auto gap = cont_.check(h);
    const bool gen_changed = !first_ && h.generation != last_gen_;
    if (first_ || gen_changed || d.flags.has(Flag::Retune) || gap || d.flags.has(Flag::Discontinuity)) {
        if (!first_ && (gen_changed || gap || d.flags.has(Flag::Discontinuity))) {
            sigmf::Discontinuity disc;
            disc.file_sample = written_;
            disc.generation = h.generation;
            disc.sample_index = h.sample_index;
            disc.missing_samples = (gap && !gen_changed) ? (gap->actual - gap->expected) : 0;
            disc.flags = to_string(d.flags);
            meta_.discontinuities.push_back(disc);
        }
        // 新しい capture segment (SigMF captures[] は retune / 不連続の両方をこれで表す)
        sigmf::Capture c;
        c.sample_start = written_;
        c.frequency = initial_freq_; // close() で tuning history から確定
        c.generation = h.generation;
        c.hw_time = h.hw_time;
        c.host_ns = h.host_ns;
        c.sample_index = h.sample_index;
        meta_.captures.push_back(c);
        first_ = false;
        last_gen_ = h.generation;
    }
    const auto bytes = d.block.bytes();
    data_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!data_) {
        ok_ = false;
        if (events_) events_->emit(EventKind::Error, name(), "write failed (disk full?)", d.block.range());
        return;
    }
    written_ += h.sample_count;
}

void SigmfRecorder::close() {
    // event 配送は非同期なので、閉じる前に未配送の Retune を取り込む
    if (events_) events_->flush();
    {
        std::lock_guard lk(mu_);
        if (closed_) return;
        closed_ = true;
        data_.close();
        meta_.total_samples = written_;
        // capture ごとの周波数を tuning history から確定(generation 内で sample_index 以下の最新)
        for (auto& c : meta_.captures) {
            for (const auto& t : meta_.tuning)
                if (t.generation == c.generation && t.sample_index <= c.sample_index) c.frequency = t.frequency;
        }
        std::string err;
        if (!sigmf::write(base_, meta_, &err) && events_) events_->emit(EventKind::Error, name(), err);
    }
    if (events_ && listener_) events_->unsubscribe(listener_);
}

} // namespace spear
