// TOKUDER の復号スレッド。VoiceTimeline の命令を順に受け取り、tokuder::Decoder(tokuder48_causal_v1)で 8 kHz の音声にする。
// 推論は DSP thread の外の専用スレッドで行う(docs/DEVELOPMENT.md、STD-T98 の secret::Worker と同じ考え方)。
// 1 チャネルの復号は FZ-G2 で 1 フレーム(20 ms)あたり約 1.6 ms(1 コアの 8 %)。
// 出力は on_audio(復号スレッドから呼ぶ): 確定したサンプルと、それを出した命令の play。
#pragma once

#include "timeline.hpp"
#include "tokuder.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <thread>

namespace spear::std_t99 {

class VoiceWorker {
public:
    using AudioFn = std::function<void(std::span<const float> pcm, bool play)>;
    explicit VoiceWorker(AudioFn on_audio, std::size_t max_queue = 4096);
    ~VoiceWorker();
    // DSP thread から。溢れたら捨てて数える(1 命令 = 20 ms。既定 4096 で約 80 s ぶん)
    void submit(const VoiceCommand& c);
    // 渡した命令をすべて復号し終えるまで待つ(テスト・ツール用)
    void drain();
    uint64_t dropped() const { return dropped_; }
    uint64_t decoded() const { return decoded_; }

private:
    void run();
    AudioFn on_audio_;
    std::size_t max_queue_;
    std::mutex mu_;
    std::condition_variable cv_, idle_cv_;
    std::deque<VoiceCommand> queue_;
    bool stop_ = false, busy_ = false;
    std::atomic<uint64_t> dropped_{0}, decoded_{0};
    std::thread th_;
};

} // namespace spear::std_t99
