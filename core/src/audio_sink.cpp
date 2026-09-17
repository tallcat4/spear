#include "spear/core/audio_sink.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace spear {

AudioSink::AudioSink(EventBus* events, std::string device, unsigned sample_rate, std::size_t ring_frames)
    : events_(events), device_(std::move(device)), rate_(sample_rate), ring_(ring_frames, 0.f) {}

AudioSink::~AudioSink() { close(); }

bool AudioSink::open(std::string* err) {
    close();
    snd_pcm_t* pcm = nullptr;
    int rc = snd_pcm_open(&pcm, device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        st_.error = std::string("snd_pcm_open: ") + snd_strerror(rc);
        if (err) *err = st_.error;
        if (events_) events_->emit(EventKind::Error, "audio", st_.error);
        return false;
    }
    // 32-bit float, mono, rate_。バッファ ~100 ms、period ~20 ms
    rc = snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, rate_, 1, 100000);
    if (rc < 0) {
        st_.error = std::string("snd_pcm_set_params: ") + snd_strerror(rc);
        if (err) *err = st_.error;
        if (events_) events_->emit(EventKind::Error, "audio", st_.error);
        snd_pcm_close(pcm);
        return false;
    }
    pcm_ = pcm;
    {
        std::lock_guard lk(mu_);
        head_ = tail_ = count_ = 0;
        st_.open = true;
        st_.error.clear();
    }
    stop_ = false;
    th_ = std::thread([this] { run(); });
    if (events_) events_->emit(EventKind::Info, "audio", "opened " + device_ + " @ " + std::to_string(rate_) + " Hz mono f32");
    return true;
}

void AudioSink::close() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    if (pcm_) {
        snd_pcm_drop(static_cast<snd_pcm_t*>(pcm_));
        snd_pcm_close(static_cast<snd_pcm_t*>(pcm_));
        pcm_ = nullptr;
    }
    std::lock_guard lk(mu_);
    st_.open = false;
}

void AudioSink::write(std::span<const float> mono) {
    std::lock_guard lk(mu_);
    if (!st_.open) return;
    for (float v : mono) {
        if (count_ == ring_.size()) { st_.overruns++; continue; }   // 満杯: 捨てる(数える)
        ring_[head_] = v;
        head_ = (head_ + 1) % ring_.size();
        ++count_;
    }
}

AudioStats AudioSink::stats() const {
    std::lock_guard lk(mu_);
    AudioStats s = st_;
    s.latency_ms = static_cast<double>(count_) / rate_ * 1e3;
    return s;
}

void AudioSink::run() {
    auto* pcm = static_cast<snd_pcm_t*>(pcm_);
    const std::size_t period = rate_ / 50;   // 20 ms
    std::vector<float> buf(period);
    uint64_t last_underrun_report = 0;
    while (!stop_) {
        std::size_t n = 0;
        {
            std::lock_guard lk(mu_);
            n = std::min(period, count_);
            const float g = mute_ ? 0.f : gain_.load();
            for (std::size_t i = 0; i < n; ++i) {
                buf[i] = std::clamp(ring_[tail_] * g, -1.f, 1.f);
                tail_ = (tail_ + 1) % ring_.size();
            }
            count_ -= n;
        }
        if (n < period) std::fill(buf.begin() + static_cast<long>(n), buf.end(), 0.f);   // 足りない分は無音
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf.data(), period);
        if (w == -EPIPE) {
            // underrun: 再生側が空になった。数えて回復する(無言にしない)
            {
                std::lock_guard lk(mu_);
                st_.underruns++;
            }
            snd_pcm_prepare(pcm);
            const auto u = stats().underruns;
            if (events_ && u - last_underrun_report >= 1 && (u < 5 || u % 50 == 0)) {
                last_underrun_report = u;
                events_->emit(EventKind::Underflow, "audio", "ALSA underrun", {}, static_cast<int64_t>(u));
            }
        } else if (w < 0) {
            snd_pcm_recover(pcm, static_cast<int>(w), 1);
        } else {
            std::lock_guard lk(mu_);
            st_.frames_written += static_cast<uint64_t>(w);
        }
    }
}

} // namespace spear
