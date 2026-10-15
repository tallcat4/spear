// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// JR 在来線デジタル列車無線のチャネル割当と路線。原本: crates/receiver-core/src/presets.rs と ui/radio-presets.json(commit dfc7440)。
//   組み込み: 原本の radio-presets.json をそのまま埋め込み(presets_json.cpp)、同じ読み込み・検査を通す。
//   外部ファイル: 同じ形式(schema_version 1)の JSON を site.conf の jr_train.presets_file で読む。全体を検査してから使う。
// 周波数は 352.53750〜352.66875 MHz、6.25 kHz 間隔(下り = 基地局送信)。
#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace spear::jr_train {

enum class Service { Voice, Data, Common };   // 音声波 / データ波 / 旅客一斉情報(共用波)

struct ChannelDef {
    double frequency_hz = 0;
    std::string label;       // 原本の表示名(日本語。例: "D5 音声第1波")
    std::string short_label; // 画面のタイル用(ASCII。例: "D5 V1")。原本の表示名から作る
    Service service = Service::Voice;
    std::string note;        // 変則割当など(日本語)
};

// 路線・区間(原本 Route)。音声 2 波とデータ 1 波を明記する
struct Route {
    std::string id, line, section, designation;
    std::array<uint64_t, 2> voice_hz{};
    uint64_t data_hz = 0;
    std::string note;
};

struct Preset {
    std::string id;          // "east" / "central" / "west"
    std::string label;       // "JR 東日本（22波）" など
    double center_hz = 0;    // 受信帯域の中心(原本 capture.center_hz)
    double sample_rate = 0;  // 原本の推奨レート(spear は B210 を 1.92 MS/s で受けて 240 kS/s に間引く)
    std::string note;
    std::vector<ChannelDef> channels;   // 周波数の順
    std::vector<Route> routes;          // このプロファイルで 3 波とも受信できる路線だけ
};

class PresetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

const std::vector<Preset>& presets();                       // 組み込み
const Preset* find_preset(std::string_view id);             // 組み込みから id で引く。無ければ nullptr
std::vector<Preset> parse_presets(std::string_view json);   // 失敗は PresetError(原本と同じ日本語のメッセージ)
std::vector<Preset> load_presets_file(const std::string& path);   // 2 MiB まで、UTF-8(BOM 可)
const char* to_string(Service s);

// 路線を選んで受信するチャネル: 選んだ路線の音声 2 波・データ 1 波(重複は 1 波)+ 共用波(include_common)。周波数の順
std::vector<ChannelDef> channels_for_routes(const Preset& p, const std::vector<std::string>& route_ids, bool include_common);

} // namespace spear::jr_train
