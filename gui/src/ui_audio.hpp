// spear-gui — UiAudio: GUI が鳴らす音(操作音・起動音)の共通出力。
//
// 音源は core の AudioSink(ALSA/PipeWire)をシェルが 1 つ持つだけ。Qt Multimedia は使わない。
// 音量は持たない(常に 100 %、OS 側で調整)。App の MUTE には影響されない。
// 波形を作るのは TapSound(タップ音・拒否音)/ BootSound(起動音)で、ここは書くだけ。
#pragma once

#include "spear/core/audio_sink.hpp"
#include "spear/core/event.hpp"

#include <span>
#include <string>

namespace spear::gui {

class UiAudio {
public:
    static constexpr unsigned kRate = 48000;

    // device: App 音声と同じ設定(--set audio_device=...)。open に失敗しても例外は出さず、Warning(source "ui")を残して無音で続く。
    // 無音を流し続ける sink の underrun は聞こえないので、AudioSink 自身のイベントは出させない(DIAG を赤で埋めない)
    explicit UiAudio(EventBus* events, const std::string& device = "default");

    void play(std::span<const float> mono);   // ノンブロッキング(ring に積むだけ。溢れた分は捨てられる)

private:
    AudioSink sink_;
};

} // namespace spear::gui
