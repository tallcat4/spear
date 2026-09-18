#include "ui_audio.hpp"

namespace spear::gui {
namespace {

// ALSA バッファ。復調音声の 100 ms ではタップから発音までが遅れて感じる。詰めすぎると FZ-G2 の熱スロットリング時に underrun が出る
constexpr unsigned kLatencyUs = 50000;
// ring は起動音(約 1.2 s)が丸ごと入る長さ
constexpr std::size_t kRingFrames = UiAudio::kRate * 2;

} // namespace

UiAudio::UiAudio(EventBus* events, const std::string& device) : sink_(nullptr, device, kRate, kRingFrames, kLatencyUs) {
    std::string err;
    if (!sink_.open(&err) && events) events->emit(EventKind::Warning, "ui", "ui sound disabled: " + err);   // GUI は無音で続く
}

void UiAudio::play(std::span<const float> mono) { sink_.write(mono); }

} // namespace spear::gui
