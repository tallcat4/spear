#include "timeline.hpp"

#include "tokuder.hpp"

namespace spear::std_t99 {

namespace {
constexpr int64_t kRfFrame = 8000;   // 80 ms at 100 kHz(Receiver の index)
}

// フレームは origin + 8000 k ± タイミングの揺れに並ぶ。流れの最初のフレームを基準に丸めるので、番号が丸めの境界に乗らない
int64_t VoiceTimeline::number(uint64_t sample) const {
    const auto delta = static_cast<int64_t>(sample) - static_cast<int64_t>(origin_);
    return (delta >= 0 ? delta + kRfFrame / 2 : delta - kRfFrame / 2) / kRfFrame;
}

void VoiceTimeline::push(const uint8_t* info, bool play) {
    VoiceCommand c;
    std::copy(info, info + 48, c.info.begin());
    c.play = last_play_ = play;
    sink_(c);
}

void VoiceTimeline::flush() {
    if (active_) sink_(VoiceCommand{VoiceCommand::Kind::Flush, {}, last_play_});   // 流れの末尾は最後のフレームと同じ扱い
    active_ = false;
    previous_.fill(0);
}

void VoiceTimeline::frame(const Frame& f, bool end_call, bool play) {
    // 同期バーストか終話で流れを終える(フレームの前に: 終話の FACCH の半分は直前の音声を繰り返すだけ)
    if (f.sync_burst() || end_call) flush();
    if (f.rich.f == 0b10) stream_frame(f, play);
}

void VoiceTimeline::stream_frame(const Frame& f, bool play) {
    const bool tch[2] = {f.halves[0].kind == HalfKind::tch, f.halves[1].kind == HalfKind::tch};
    if (active_) {
        const int64_t n = number(f.input_sample_index);
        if (n <= last_frame_) return;   // 重複か順序の逆転
        if (n - last_frame_ > kMaxGap) flush();
        else { erased((n - last_frame_ - 1) * 4, play); last_frame_ = n; }
    }
    if (!active_) {
        if (!tch[0] && !tch[1]) return;
        active_ = true;
        origin_ = f.input_sample_index;
        last_frame_ = 0;
    }
    for (int half = 0; half < 2; ++half) {
        if (!tch[half]) { erased(2, play); continue; }
        const auto& h = f.halves[static_cast<std::size_t>(half)];
        for (int slot = 0; slot < 2; ++slot) {
            tokuder::soft_decode(h.llr.data() + 72 * slot, previous_.data());
            push(previous_.data(), play);
        }
    }
}

void VoiceTimeline::idle(uint64_t sample) {
    if (active_ && number(sample) > last_frame_ + kMaxGap + 1) flush();
}

} // namespace spear::std_t99
