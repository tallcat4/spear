// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// JR 在来線デジタル列車無線のチャネル割当。原本: ui/radio-presets.json(commit dfc7440)の profiles と channel_sets。
// 路線(routes)は持たない。周波数は 352.53750〜352.66875 MHz、6.25 kHz 間隔(下り = 基地局送信)。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace spear::jr_train {

enum class Service { Voice, Data, Common };   // 音声波 / データ波 / 旅客一斉情報(共用波)

struct ChannelDef {
    double frequency_hz = 0;
    std::string label;       // 原本の表示名(日本語。例: "D5 音声第1波")
    std::string short_label; // 画面のタイル用(ASCII。例: "D5 V1")
    Service service = Service::Voice;
    std::string note;        // 変則割当など(日本語)
};

struct Preset {
    std::string id;          // "east" / "central" / "west"
    std::string label;       // "JR 東日本(22波)" など
    double center_hz = 0;    // 受信帯域の中心(原本 capture.center_hz)
    std::vector<ChannelDef> channels;
};

const std::vector<Preset>& presets();
// id で引く。無ければ nullptr
const Preset* find_preset(std::string_view id);
const char* to_string(Service s);

} // namespace spear::jr_train
