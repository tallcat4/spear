// spear-gui — BootSound: 起動音。立ち上げ(Source::warm_up)が終わりスプラッシュが明けるときに 1 回。
// シェルは durationMs だけ待ってからメニューへ移る(起動ログを読む時間にもなる)。
#pragma once

#include "ui_audio.hpp"

#include <QObject>

#include <vector>

namespace spear::gui {

class BootSound : public QObject {
    Q_OBJECT
    Q_PROPERTY(int durationMs READ durationMs CONSTANT)
public:
    explicit BootSound(UiAudio& out, QObject* parent = nullptr);

    Q_INVOKABLE void play();
    int durationMs() const { return static_cast<int>(melody_.size() * 1000 / UiAudio::kRate); }

private:
    UiAudio& out_;
    std::vector<float> melody_;
};

} // namespace spear::gui
