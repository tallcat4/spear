#include "worker.hpp"

#include <vector>

namespace spear::std_t99 {

VoiceWorker::VoiceWorker(AudioFn on_audio, std::size_t max_queue) : on_audio_(std::move(on_audio)), max_queue_(max_queue) {
    tokuder::model();   // 重みの読み込み(埋め込みの並べ直し)を先に済ませる
    th_ = std::thread([this] { run(); });
}

VoiceWorker::~VoiceWorker() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void VoiceWorker::submit(const VoiceCommand& c) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.size() >= max_queue_) { ++dropped_; return; }
        queue_.push_back(c);
    }
    cv_.notify_one();
}

void VoiceWorker::drain() {
    std::unique_lock<std::mutex> lk(mu_);
    idle_cv_.wait(lk, [this] { return queue_.empty() && !busy_; });
}

void VoiceWorker::run() {
    tokuder::Decoder decoder(tokuder::model());
    std::vector<float> pcm;
    for (;;) {
        VoiceCommand c;
        {
            std::unique_lock<std::mutex> lk(mu_);
            busy_ = false;
            if (queue_.empty()) idle_cv_.notify_all();
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            c = queue_.front();
            queue_.pop_front();
            busy_ = true;
        }
        pcm.clear();
        if (c.kind == VoiceCommand::Kind::Flush) decoder.flush(pcm);
        else { decoder.push(c.info.data(), pcm); ++decoded_; }
        if (!pcm.empty() && on_audio_) on_audio_(pcm, c.play);
    }
}

} // namespace spear::std_t99
