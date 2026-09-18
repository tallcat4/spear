// spear-gui — TapSound: GUI の操作音(タップ音 / 拒否音)。
//
// 意味のある入力(ボタン等)がアクションとして受け付けられた瞬間にだけ鳴らす。空白のタップ・モーダルの背景・
// 無効キー・ドラッグ/ピンチのような連続操作では鳴らさない(呼ぶ側の規約: QML の Spear.Input Feedback singleton)。
// 音源は core の AudioSink(ALSA/PipeWire)をシェルが 1 つ持つだけ。Qt Multimedia は使わない。
// 音量は持たない(常に 100 %、OS 側で調整)。App の MUTE には影響されない。
#pragma once

#include "spear/core/audio_sink.hpp"
#include "spear/core/event.hpp"

#include <QObject>

#include <string>
#include <vector>

namespace spear::gui {

class TapSound : public QObject {
    Q_OBJECT
public:
    // device: App 音声と同じ設定(--set audio_device=...)。open に失敗しても例外は出さず、Warning(source "ui")を残して無音で続く。
    // 無音を流し続ける sink の underrun は聞こえないので、AudioSink 自身のイベントは出させない(DIAG を赤で埋めない)
    explicit TapSound(EventBus* events, const std::string& device = "default", QObject* parent = nullptr);
    ~TapSound() override;

    Q_INVOKABLE void tap();      // 入力を受け付けた
    Q_INVOKABLE void reject();   // 数値入力を拒否した(赤で理由が出る事象と 1:1)

private:
    AudioSink sink_;
    std::vector<float> tap_, reject_;
};

} // namespace spear::gui
