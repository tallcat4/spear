// S.P.E.A.R. core — AudioSink (要件 §7 Sink 抽象の音声版)
//
// ALSA(PipeWire の ALSA 互換層経由)。自前 thread が ring buffer から PCM へ書く。
// 不変条件は TX と同じ (§10): 全 sample を出すか、underrun を数えて event にするか。無言の欠落は無い。
// GUI/DSP thread は write() でブロックしない(ring が満杯なら捨てて overrun を数える)。
// 音量は持たない(常に 100 %、MUTE のみ)。音量は OS 側(PipeWire)で調整する。将来キオスク化するときも
// 音量はシステムで一貫して 1 か所に置く(App ごとの音量は持たない)。
#pragma once

#include "event.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace spear {

struct AudioStats {
    uint64_t frames_written = 0;   // PCM へ渡した frame 数
    uint64_t underruns = 0;        // ALSA の underrun(再生側が空)
    uint64_t overruns = 0;         // ring が満杯で捨てた frame 数
    double   latency_ms = 0;       // ring の滞留(推定)
    bool     open = false;
    std::string error;
};

class AudioSink {
public:
    // device: "default" 等。mono float を渡す。latency_us は ALSA 側バッファ(復調音声は 100 ms、UI 音のような
    // 即応が要るものは短く。再生 thread の period もこれに合わせて短くなる)。events は nullptr 可(何も報告しない)。
    AudioSink(EventBus* events, std::string device = "default", unsigned sample_rate = 48000,
              std::size_t ring_frames = 48000 / 4, unsigned latency_us = 100000);
    ~AudioSink();
    bool open(std::string* err = nullptr);
    void close();
    void write(std::span<const float> mono);   // ノンブロッキング
    void set_mute(bool m) { mute_ = m; }
    AudioStats stats() const;
    unsigned sample_rate() const { return rate_; }

private:
    void run();
    EventBus* events_;
    std::string device_;
    unsigned rate_;
    unsigned latency_us_;
    void* pcm_ = nullptr;      // snd_pcm_t*
    std::vector<float> ring_;
    std::size_t head_ = 0, tail_ = 0, count_ = 0;   // mu_ で保護
    mutable std::mutex mu_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> mute_{false};
    AudioStats st_;
};

} // namespace spear
