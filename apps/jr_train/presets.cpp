// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: ui/radio-presets.json(commit dfc7440)。表の値は原本の JSON から転記した(周波数・名称・種別・注記)。
#include "presets.hpp"

namespace spear::jr_train {
namespace {

ChannelDef v(double hz, const char* label, const char* short_label, const char* note = "") { return {hz, label, short_label, Service::Voice, note}; }
ChannelDef d(double hz, const char* label, const char* short_label) { return {hz, label, short_label, Service::Data, ""}; }
ChannelDef c(double hz) { return {hz, "旅客一斉情報", "COMMON", Service::Common, ""}; }

std::vector<Preset> make() {
    std::vector<Preset> p;
    p.push_back({"east", "JR 東日本(22波)", 352'603'125.0, {
        v(352'537'500, "D5 音声第1波", "D5 V1", "変則割当：大船渡線 D7 音声第1波"),
        v(352'543'750, "D7 音声第1波", "D7 V1", "変則割当：奥羽本線（大館～青森）D6 音声第1波 / 仙石線 D2 音声第1波"),
        v(352'550'000, "D3 音声第1波", "D3 V1"),
        v(352'556'250, "D1 音声第1波", "D1 V1"),
        v(352'562'500, "D4 音声第1波", "D4 V1", "変則割当：仙山線 D3 音声第2波"),
        v(352'568'750, "D6 音声第1波", "D6 V1", "変則割当：東北本線（黒磯～盛岡）D1 音声第2波"),
        v(352'575'000, "D2 音声第1波", "D2 V1"),
        v(352'581'250, "D6 音声第2波", "D6 V2", "変則割当：陸羽東線 D3 音声第2波 / 大船渡線 D7 音声第2波"),
        v(352'587'500, "D5 音声第2波", "D5 V2"),
        v(352'593'750, "D2 音声第2波", "D2 V2"),
        v(352'600'000, "D4 音声第2波", "D4 V2"),
        v(352'606'250, "D7 音声第2波", "D7 V2", "変則割当：仙石線 D2 音声第2波 / 石巻線 D5 音声第2波"),
        v(352'612'500, "D3 音声第2波", "D3 V2", "変則割当：中央本線（甲府～小淵沢）D6 音声第1波"),
        v(352'618'750, "D1 音声第2波", "D1 V2", "変則割当：小海線 D4 音声第1波"),
        c(352'625'000),
        d(352'631'250, "D1 データ波", "D1 DATA"),
        d(352'637'500, "D2 データ波", "D2 DATA"),
        d(352'643'750, "D3 データ波", "D3 DATA"),
        d(352'650'000, "D4 データ波", "D4 DATA"),
        d(352'656'250, "D5 データ波", "D5 DATA"),
        d(352'662'500, "D6 データ波", "D6 DATA"),
        d(352'668'750, "D7 データ波", "D7 DATA"),
    }});
    p.push_back({"central", "JR 東海(3波)", 352'625'000.0, {
        v(352'600'000, "D4 音声第1波", "D4 V1"),
        v(352'637'500, "D4 音声第2波", "D4 V2"),
        d(352'650'000, "D4 データ波", "D4 DATA"),
    }});
    p.push_back({"west", "JR 西日本(19波)", 352'603'125.0, {
        v(352'537'500, "D2 音声第1波", "D2 V1"),
        c(352'543'750),
        v(352'550'000, "D5 音声第2波", "D5 V2"),
        v(352'556'250, "D4 音声第2波", "D4 V2"),
        v(352'562'500, "D2 音声第2波", "D2 V2"),
        v(352'568'750, "D6 音声第2波", "D6 V2"),
        v(352'575'000, "D5 音声第1波", "D5 V1"),
        v(352'581'250, "D3 音声第2波", "D3 V2"),
        v(352'587'500, "D7 音声第2波", "D7 V2"),
        v(352'593'750, "D7 音声第1波", "D7 V1"),
        v(352'600'000, "D4 音声第1波", "D4 V1"),
        v(352'606'250, "D6 音声第1波", "D6 V1"),
        v(352'612'500, "D3 音声第1波", "D3 V1"),
        d(352'637'500, "D2 データ波", "D2 DATA"),
        d(352'643'750, "D3 データ波", "D3 DATA"),
        d(352'650'000, "D4 データ波", "D4 DATA"),
        d(352'656'250, "D5 データ波", "D5 DATA"),
        d(352'662'500, "D6 データ波", "D6 DATA"),
        d(352'668'750, "D7 データ波", "D7 DATA"),
    }});
    return p;
}

} // namespace

const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = make();
    return p;
}

const Preset* find_preset(std::string_view id) {
    for (const auto& p : presets())
        if (p.id == id) return &p;
    return nullptr;
}

const char* to_string(Service s) {
    switch (s) {
    case Service::Voice: return "voice";
    case Service::Data: return "data";
    case Service::Common: return "common";
    }
    return "?";
}

} // namespace spear::jr_train
