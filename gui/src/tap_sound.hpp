// spear-gui — TapSound: GUI の操作音(タップ音 / 拒否音)。
//
// 意味のある入力(ボタン等)がアクションとして受け付けられた瞬間にだけ鳴らす。空白のタップ・モーダルの背景・
// 無効キー・ドラッグ/ピンチのような連続操作では鳴らさない(呼ぶ側の規約: QML の Spear.Input Feedback singleton)。
#pragma once

#include "ui_audio.hpp"

#include <QObject>

#include <vector>

namespace spear::gui {

class TapSound : public QObject {
    Q_OBJECT
public:
    explicit TapSound(UiAudio& out, QObject* parent = nullptr);

    // 同じイベントループ 1 周の中で tap() と reject() が両方来たら拒否音だけ鳴らす(ENTER のタップ → 即拒否、のとき
    // 「カチ + ブー」にしない)。実際に書くのは次のループで 1 回
    Q_INVOKABLE void tap();      // 入力を受け付けた
    Q_INVOKABLE void reject();   // 数値入力を拒否した(赤で理由が出る事象と 1:1)

private:
    void flush();
    UiAudio& out_;
    std::vector<float> tap_, reject_;
    bool tap_pending_ = false, reject_pending_ = false, flush_scheduled_ = false;
};

} // namespace spear::gui
