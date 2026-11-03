// S.P.E.A.R. appfw — App の音声のレベル合わせの設定(dsp::Leveler)。docs/audio.md
//   site.conf: audio_level_dbfs(声のある所の rms の目標、既定 −18。"off" で掛けない)。audio_device と同じく全 App 共通のキー。
//   App は AudioSink に渡す直前に Leveler を通す(保存・Stream Bus に出す音声には掛けない)。
#pragma once

#include "spear/dsp/leveler.hpp"

#include <QVariantMap>

#include <algorithm>
#include <memory>
#include <optional>

namespace spear::appfw {

// 前の値に settings を重ねる(キーが無ければそのまま)
inline std::optional<dsp::LevelerConfig> speech_level(const QVariantMap& settings, std::optional<dsp::LevelerConfig> current) {
    if (!settings.contains("audio_level_dbfs")) return current;
    const QString v = settings["audio_level_dbfs"].toString().trimmed();
    if (v.compare("off", Qt::CaseInsensitive) == 0) return std::nullopt;
    bool ok = false;
    const double db = v.toDouble(&ok);
    if (!ok) return current;
    dsp::LevelerConfig c = current.value_or(dsp::LevelerConfig{});
    c.target_dbfs = std::clamp(db, -40.0, -6.0);
    return c;
}

inline std::unique_ptr<dsp::Leveler> make_speech_leveler(const std::optional<dsp::LevelerConfig>& cfg, const char* name, double rate) {
    return cfg ? std::make_unique<dsp::Leveler>(name, rate, *cfg) : nullptr;
}

} // namespace spear::appfw
