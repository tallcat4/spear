// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/receiver-core/src/presets.rs。JSON の読み込みは最小限の自前実装(spear は外部の JSON ライブラリに依存しない)。
#include "presets.hpp"

#include "dsp/jr_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>

namespace spear::jr_train {

extern const char* const kBuiltinPresetsJson;   // presets_json.cpp

namespace {

// ---- 最小の JSON 読み込み ----
struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    bool integer = false;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;
    const Value* get(const std::string& k) const {
        for (const auto& [key, v] : o)
            if (key == k) return &v;
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}
    Value parse() {
        Value v = value();
        ws();
        if (p_ != t_.size()) fail();
        return v;
    }

private:
    [[noreturn]] void fail() { throw PresetError("JSON の形式が正しくありません(位置 " + std::to_string(p_) + ")"); }
    void ws() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
    }
    bool eat(char c) {
        ws();
        if (p_ < t_.size() && t_[p_] == c) {
            ++p_;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!eat(c)) fail();
    }
    static void utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    uint32_t hex4() {
        if (p_ + 4 > t_.size()) fail();
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail();
        }
        return v;
    }
    std::string string() {
        expect('"');
        std::string out;
        while (p_ < t_.size() && t_[p_] != '"') {
            char c = t_[p_++];
            if (static_cast<unsigned char>(c) < 0x20) fail();
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p_ >= t_.size()) fail();
            c = t_[p_++];
            switch (c) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t u = hex4();
                if (u >= 0xd800 && u < 0xdc00) {
                    if (p_ + 2 > t_.size() || t_[p_] != '\\' || t_[p_ + 1] != 'u') fail();
                    p_ += 2;
                    const uint32_t lo = hex4();
                    if (lo < 0xdc00 || lo >= 0xe000) fail();
                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                } else if (u >= 0xdc00 && u < 0xe000) {
                    fail();
                }
                utf8(out, u);
                break;
            }
            default: fail();
            }
        }
        if (p_ >= t_.size()) fail();
        ++p_;
        return out;
    }
    Value value() {
        ws();
        if (p_ >= t_.size()) fail();
        Value v;
        const char c = t_[p_];
        if (c == '{') {
            ++p_;
            v.kind = Value::Object;
            if (eat('}')) return v;
            do {
                ws();
                std::string k = string();
                expect(':');
                v.o.emplace_back(std::move(k), value());
            } while (eat(','));
            expect('}');
        } else if (c == '[') {
            ++p_;
            v.kind = Value::Array;
            if (eat(']')) return v;
            do v.a.push_back(value());
            while (eat(','));
            expect(']');
        } else if (c == '"') {
            v.kind = Value::String;
            v.s = string();
        } else if (t_.substr(p_, 4) == "true") {
            p_ += 4;
            v.kind = Value::Bool;
            v.b = true;
        } else if (t_.substr(p_, 5) == "false") {
            p_ += 5;
            v.kind = Value::Bool;
        } else if (t_.substr(p_, 4) == "null") {
            p_ += 4;
        } else {
            const std::size_t start = p_;
            if (t_[p_] == '-') ++p_;
            bool integer = true;
            while (p_ < t_.size() && ((t_[p_] >= '0' && t_[p_] <= '9') || t_[p_] == '.' || t_[p_] == 'e' || t_[p_] == 'E' || t_[p_] == '+' || t_[p_] == '-')) {
                if (t_[p_] == '.' || t_[p_] == 'e' || t_[p_] == 'E') integer = false;
                ++p_;
            }
            if (p_ == start) fail();
            v.kind = Value::Number;
            v.integer = integer;
            v.n = std::stod(std::string(t_.substr(start, p_ - start)));
        }
        return v;
    }
    std::string_view t_;
    std::size_t p_ = 0;
};

// 原本は serde の deny_unknown_fields: 知らないキーは誤り
void only(const Value& v, std::initializer_list<const char*> keys, const std::string& what) {
    if (v.kind != Value::Object) throw PresetError(what + " はオブジェクトにしてください");
    for (const auto& [k, x] : v.o)
        if (std::find_if(keys.begin(), keys.end(), [&](const char* s) { return k == s; }) == keys.end()) throw PresetError(what + ": 未知の項目 " + k);
}
const Value& need(const Value& v, const char* key, const std::string& what) {
    const Value* x = v.get(key);
    if (!x) throw PresetError(what + ": " + key + " がありません");
    return *x;
}
std::string str(const Value& v, const char* key, const std::string& what, bool optional = false) {
    const Value* x = v.get(key);
    if (!x) {
        if (optional) return {};
        throw PresetError(what + ": " + key + " がありません");
    }
    if (x->kind != Value::String) throw PresetError(what + ": " + key + " は文字列にしてください");
    return x->s;
}
uint64_t u64(const Value& x, const std::string& what) {
    if (x.kind != Value::Number || !x.integer || x.n < 0 || x.n > 1.8e19) throw PresetError(what + " は 0 以上の整数にしてください");
    return static_cast<uint64_t>(x.n);
}
bool valid_id(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; });
}
std::size_t chars(const std::string& s) {   // UTF-8 の文字数
    return static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xc0) != 0x80; }));
}
bool text(const std::string& s) {
    const bool blank = std::all_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
    return !blank && chars(s) <= 200;
}
Service service_of(const std::string& s, const std::string& what) {
    if (s == "voice") return Service::Voice;
    if (s == "data") return Service::Data;
    if (s == "common") return Service::Common;
    throw PresetError(what + ": service は voice / data / common にしてください");
}

// 原本の表示名からタイル用の短い名前を作る("D5 音声第1波" → "D5 V1"、"D1 データ波" → "D1 DATA"、共用波 → "COMMON")
std::string short_label(const std::string& label, Service s, uint64_t hz) {
    if (s == Service::Common) return "COMMON";
    const auto sp = label.find(' ');
    const std::string d = sp == std::string::npos ? std::string() : label.substr(0, sp);
    const bool designation = d.size() >= 2 && d[0] == 'D' && std::all_of(d.begin() + 1, d.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (designation) {
        if (s == Service::Data) return d + " DATA";
        if (label.find("第1波") != std::string::npos) return d + " V1";
        if (label.find("第2波") != std::string::npos) return d + " V2";
        return d + " V";
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.5f", static_cast<double>(hz) / 1e6);
    return buf;
}

struct RawChannel {
    uint64_t hz;
    std::string label;
    Service service;
    std::string note;
};
struct RawSet {
    std::string id;
    std::vector<RawChannel> channels;
    std::vector<Route> routes;
};

} // namespace

std::vector<Preset> parse_presets(std::string_view json) {
    if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xef && static_cast<unsigned char>(json[1]) == 0xbb && static_cast<unsigned char>(json[2]) == 0xbf) json.remove_prefix(3);
    const Value root = Parser(json).parse();
    const Value* version = root.get("schema_version");
    if (root.kind != Value::Object || !version || version->kind != Value::Number || version->n != 1) throw PresetError("受信設定の schema_version は 1 を指定してください");
    only(root, {"schema_version", "channel_sets", "profiles"}, "受信設定");
    const Value& sets_v = need(root, "channel_sets", "受信設定");
    const Value& profiles_v = need(root, "profiles", "受信設定");
    if (sets_v.kind != Value::Array || profiles_v.kind != Value::Array || sets_v.a.empty() || profiles_v.a.empty())
        throw PresetError("チャンネル定義と受信プロファイルを 1 件以上指定してください");
    std::vector<RawSet> sets;
    std::set<std::string> set_ids;
    for (const auto& sv : sets_v.a) {
        only(sv, {"id", "region", "direction", "channels", "routes"}, "チャンネル定義");
        RawSet set;
        set.id = str(sv, "id", "チャンネル定義");
        if (!valid_id(set.id) || !set_ids.insert(set.id).second) throw PresetError("チャンネル定義の ID が不正または重複しています");
        str(sv, "region", set.id);
        if (str(sv, "direction", set.id) != "downlink") throw PresetError(set.id + ": 移動局からの上り復号は未対応です");
        const Value& chans = need(sv, "channels", set.id);
        if (chans.kind != Value::Array || chans.a.empty() || chans.a.size() > 64) throw PresetError("各チャンネル定義は 1–64 波にしてください");
        std::set<uint64_t> freqs;
        for (const auto& cv : chans.a) {
            only(cv, {"frequency_hz", "label", "service", "note"}, set.id);
            RawChannel c{u64(need(cv, "frequency_hz", set.id), set.id + " frequency_hz"), str(cv, "label", set.id), service_of(str(cv, "service", set.id), set.id),
                         str(cv, "note", set.id, true)};
            if (c.hz == 0 || c.hz > 100'000'000'000ull || !freqs.insert(c.hz).second || !text(c.label))
                throw PresetError(set.id + ": 周波数の範囲・重複、またはラベルを確認してください");
            set.channels.push_back(std::move(c));
        }
        // 路線(原本 validate_routes)
        const std::string invalid = set.id + ": 路線の ID・区間・周波数の参照を確認してください";
        if (const Value* rv = sv.get("routes")) {
            if (rv->kind != Value::Array || rv->a.size() > 512) throw PresetError(invalid);
            std::set<std::string> ids;
            const auto has = [&](uint64_t hz, Service s) {
                return std::any_of(set.channels.begin(), set.channels.end(), [&](const RawChannel& c) { return c.hz == hz && c.service == s; });
            };
            for (const auto& r : rv->a) {
                only(r, {"id", "line", "section", "designation", "voice_hz", "data_hz", "note"}, invalid);
                Route route;
                route.id = str(r, "id", invalid);
                route.line = str(r, "line", invalid);
                route.section = str(r, "section", invalid);
                route.designation = str(r, "designation", invalid);
                route.note = str(r, "note", invalid, true);
                const Value& vh = need(r, "voice_hz", invalid);
                if (vh.kind != Value::Array || vh.a.size() != 2) throw PresetError(invalid);
                route.voice_hz = {u64(vh.a[0], invalid), u64(vh.a[1], invalid)};
                route.data_hz = u64(need(r, "data_hz", invalid), invalid);
                if (!valid_id(route.id) || !ids.insert(route.id).second || !text(route.line) || !text(route.section) || !text(route.designation) || chars(route.note) > 500 ||
                    route.voice_hz[0] == route.voice_hz[1] || !has(route.voice_hz[0], Service::Voice) || !has(route.voice_hz[1], Service::Voice) || !has(route.data_hz, Service::Data))
                    throw PresetError(invalid);
                set.routes.push_back(std::move(route));
            }
        }
        sets.push_back(std::move(set));
    }
    std::vector<Preset> result;
    std::set<std::string> profile_ids;
    for (const auto& pv : profiles_v.a) {
        only(pv, {"id", "label", "channel_set", "services", "capture", "note"}, "プロファイル");
        Preset p;
        p.id = str(pv, "id", "プロファイル");
        p.label = str(pv, "label", "プロファイル");
        p.note = str(pv, "note", p.id, true);
        if (!valid_id(p.id) || !profile_ids.insert(p.id).second || !text(p.label)) throw PresetError("プロファイルの ID・ラベルを確認してください");
        const std::string set_id = str(pv, "channel_set", p.id);
        const auto set = std::find_if(sets.begin(), sets.end(), [&](const RawSet& s) { return s.id == set_id; });
        if (set == sets.end()) throw PresetError(p.id + ": 存在しないチャンネル定義です");
        const Value& sv = need(pv, "services", p.id);
        std::set<Service> services;
        if (sv.kind != Value::Array) throw PresetError("受信対象の種別が空または重複しています");
        for (const auto& s : sv.a) {
            if (s.kind != Value::String) throw PresetError("受信対象の種別が空または重複しています");
            services.insert(service_of(s.s, p.id));
        }
        if (services.empty() || services.size() != sv.a.size()) throw PresetError("受信対象の種別が空または重複しています");
        const Value& cap = need(pv, "capture", p.id);
        only(cap, {"center_hz", "sample_rate"}, p.id);
        const uint64_t center = u64(need(cap, "center_hz", p.id), p.id);
        const uint64_t rate = u64(need(cap, "sample_rate", p.id), p.id);
        if (center == 0 || center > 100'000'000'000ull || rate < 9600 || rate > 20'000'000) throw PresetError("中心周波数またはサンプルレートが範囲外です");
        p.center_hz = static_cast<double>(center);
        p.sample_rate = static_cast<double>(rate);
        for (const auto& c : set->channels)
            if (services.count(c.service)) p.channels.push_back({static_cast<double>(c.hz), c.label, short_label(c.label, c.service, c.hz), c.service, c.note});
        std::sort(p.channels.begin(), p.channels.end(), [](const ChannelDef& a, const ChannelDef& b) { return a.frequency_hz < b.frequency_hz; });
        // 原本 validate_channels: 1–64 波、原本の推奨レートで帯域に入ること
        if (p.channels.empty() || p.channels.size() > 64) throw PresetError(p.id + ": Select 1–64 receive channels");
        for (const auto& c : p.channels)
            if (auto e = dsp::validate(p.sample_rate, c.frequency_hz - p.center_hz); !e.empty()) throw PresetError(p.id + ": " + c.label + ": " + e);
        const auto in_profile = [&](uint64_t hz) { return std::any_of(p.channels.begin(), p.channels.end(), [&](const ChannelDef& c) { return c.frequency_hz == static_cast<double>(hz); }); };
        for (const auto& r : set->routes)
            if (in_profile(r.voice_hz[0]) && in_profile(r.voice_hz[1]) && in_profile(r.data_hz)) p.routes.push_back(r);
        result.push_back(std::move(p));
    }
    return result;
}

std::vector<Preset> load_presets_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw PresetError(path + ": 通常の JSON ファイルを選択してください。");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.size() > 2 * 1024 * 1024) throw PresetError(path + ": プリセットファイルは 2 MiB 以下にしてください。");
    try {
        return parse_presets(text);
    } catch (const PresetError& e) {
        throw PresetError(path + ":" + e.what());
    }
}

const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = parse_presets(kBuiltinPresetsJson);
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

std::vector<ChannelDef> channels_for_routes(const Preset& p, const std::vector<std::string>& route_ids, bool include_common) {
    std::set<uint64_t> want;
    for (const auto& id : route_ids)
        for (const auto& r : p.routes)
            if (r.id == id) {
                want.insert(r.voice_hz[0]);
                want.insert(r.voice_hz[1]);
                want.insert(r.data_hz);
            }
    std::vector<ChannelDef> out;
    for (const auto& c : p.channels)
        if (want.count(static_cast<uint64_t>(c.frequency_hz)) || (include_common && c.service == Service::Common)) out.push_back(c);
    return out;
}

} // namespace spear::jr_train
