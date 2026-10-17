// 通話の自動選択と保存のテスト(原本 monitor.rs / recording.rs のテストの移植)
#include "call_monitor.hpp"
#include "presets.hpp"
#include "storage.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace {

using namespace spear::jr_train;
using A = CallMonitor::Activity;

// ---- monitor.rs ----

CallMonitor monitor() { return CallMonitor({"a", "b", "c"}); }
void advance(CallMonitor& m, uint64_t t) {
    for (std::size_t i = 0; i < 3; ++i) m.update(i, t, {});
}
std::vector<std::string> list(std::initializer_list<const char*> v) { return {v.begin(), v.end()}; }

TEST(JrTrainCallMonitor, EndOfInputReleasesOnsetsInsideAcquisitionLookahead) {
    auto m = monitor();
    m.update(1, 400'000, {{A::Voice, 350'000}});
    advance(m, 400'000);
    EXPECT_FALSE(m.snapshot().first);
    m.finish();
    EXPECT_EQ(m.snapshot().first.value_or(""), "b");
    for (std::size_t i = 0; i < 3; ++i) m.age(i, std::chrono::seconds(2));
    EXPECT_FALSE(m.snapshot().first);
}

TEST(JrTrainCallMonitor, WorkerOrderDoesNotChangeFifoAndFinishedWaitersAreSkipped) {
    auto m = monitor();
    m.update(1, 400'000, {{A::Voice, 200'000}});
    EXPECT_FALSE(m.snapshot().first);   // 他のチャネルがまだ追いついていない
    m.update(0, 400'000, {{A::Voice, 100'000}});
    m.update(2, 400'000, {{A::Voice, 250'000}});
    EXPECT_EQ(m.snapshot().second, list({"a", "b", "c"}));
    m.update(0, 1'000'000, {{A::Voice, 900'000}});
    m.update(2, 1'000'000, {{A::Voice, 950'000}});
    advance(m, 1'500'000);   // b は待っている間に終わった
    EXPECT_EQ(m.snapshot().second, list({"a", "c"}));
    m.update(2, 2'200'000, {{A::Voice, 1'600'000}});
    advance(m, 2'200'000);
    EXPECT_EQ(m.snapshot().first.value_or(""), "c");
    advance(m, 3'000'000);
    EXPECT_FALSE(m.snapshot().first);
}

TEST(JrTrainCallMonitor, ShortGapsKeepPriorityButNewCallsAndResetsRejoinAtTheEnd) {
    auto m = monitor();
    m.update(0, 400'000, {{A::Voice, 0}, {A::Voice, 200'000}});
    m.update(1, 400'000, {{A::Voice, 100'000}});
    advance(m, 500'000);
    EXPECT_EQ(m.snapshot().first.value_or(""), "a");
    m.update(0, 800'000, {{A::Reset, 500'000}, {A::Voice, 600'000}});
    advance(m, 800'000);
    EXPECT_EQ(m.snapshot().second, list({"b", "a"}));
    m.update(1, 2'000'000, {{A::Voice, 1'200'000}});
    m.update(0, 2'000'000, {{A::Voice, 1'000'000}, {A::Voice, 1'700'000}});
    advance(m, 2'000'000);
    EXPECT_EQ(m.snapshot().second, list({"a", "b"}));
}

// ---- recording.rs ----

struct Temp {
    std::filesystem::path p;
    Temp() {
        static int next = 0;
        p = std::filesystem::temp_directory_path() / ("jr-recording-" + std::to_string(::getpid()) + "-" + std::to_string(next++));
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
    }
    ~Temp() { std::filesystem::remove_all(p); }
};

std::vector<ChannelConfig> east(std::size_t n) {
    std::vector<ChannelConfig> c;
    for (std::size_t i = 0; i < n; ++i) c.push_back(channel_config(find_preset("east")->channels[i]));
    return c;
}
Audio pcm(const ChannelConfig& c, uint64_t epoch, uint32_t index, int16_t value) {
    Audio a;
    a.channel = c.id;
    a.epoch = epoch;
    a.index = index;
    a.pcm.fill(value);
    return a;
}
std::vector<int16_t> read_pcm(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> b{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    EXPECT_GE(b.size(), 44u);
    EXPECT_EQ(std::string(b.data(), 4), "RIFF");
    EXPECT_EQ(std::string(b.data() + 8, 8), "WAVEfmt ");
    uint32_t riff, rate, data;
    std::memcpy(&riff, b.data() + 4, 4);
    std::memcpy(&rate, b.data() + 24, 4);
    std::memcpy(&data, b.data() + 40, 4);
    EXPECT_EQ(riff, b.size() - 8);
    EXPECT_EQ(rate, 8000u);
    EXPECT_EQ(data, b.size() - 44);
    std::vector<int16_t> out((b.size() - 44) / 2);
    std::memcpy(out.data(), b.data() + 44, out.size() * 2);
    return out;
}
std::vector<int16_t> rep(int16_t v, std::size_t n) { return std::vector<int16_t>(n, v); }
std::vector<int16_t> cat(std::initializer_list<std::vector<int16_t>> parts) {
    std::vector<int16_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
std::size_t count_lines(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::size_t n = 0;
    for (std::string l; std::getline(f, l);) ++n;
    return n;
}

TEST(JrTrainStorage, MultichannelPcmWithGapsAndResynchronizationInSeparateFiles) {
    Temp temp;
    const auto channels = east(2);
    Recorder r;
    const auto status = r.start(temp.p, channels);
    const std::filesystem::path dir = *status.directory;
    EXPECT_THROW(r.start(temp.p, channels), std::runtime_error);
    r.push(pcm(channels[0], 0, 10, 120));
    r.push(pcm(channels[1], 0, 20, -240));
    r.push(pcm(channels[0], 0, 12, 360));   // 1 フレームの欠落は無音
    r.push(pcm(channels[0], 1, 0, 480));    // epoch が変われば別の WAV
    const auto s = r.stop();
    EXPECT_EQ(s.state, "completed");
    EXPECT_EQ(s.files, 3u);
    EXPECT_EQ(s.pcm_samples, 1280u);
    EXPECT_EQ(read_pcm(dir / "352537500Hz-000001.wav"), cat({rep(120, 320), rep(0, 320), rep(360, 320)}));
    EXPECT_EQ(read_pcm(dir / "352543750Hz-000001.wav"), rep(-240, 320));
    EXPECT_EQ(read_pcm(dir / "352537500Hz-000002.wav"), rep(480, 320));
    EXPECT_FALSE(std::filesystem::exists(dir / "INCOMPLETE"));
    EXPECT_EQ(count_lines(dir / "files.jsonl"), 3u);
    EXPECT_TRUE(std::filesystem::exists(dir / "session.json"));
    EXPECT_TRUE(std::filesystem::exists(dir / "result.json"));
    const auto next = r.start(temp.p, channels);
    EXPECT_NE(*next.directory, *s.directory);
    const auto no_audio = r.stop();
    EXPECT_EQ(no_audio.files, 0u);
    EXPECT_EQ(no_audio.state, "completed");
}

TEST(JrTrainStorage, FailedFileCreationKeepsExistingDataAndMarksIncomplete) {
    Temp temp;
    const auto channels = east(1);
    Recorder r;
    EXPECT_THROW(r.start(temp.p / "missing", channels), std::runtime_error);
    EXPECT_EQ(r.snapshot().state, "idle");
    const std::filesystem::path dir = *r.start(temp.p, channels).directory;
    const auto file = dir / "352537500Hz-000001.wav";
    std::ofstream(file) << "keep existing file";
    r.push(pcm(channels[0], 0, 0, 1));
    const auto s = r.stop();
    EXPECT_EQ(s.state, "failed");
    EXPECT_TRUE(s.error.has_value());
    EXPECT_EQ(s.files, 0u);
    std::ifstream f(file);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "keep existing file");
    EXPECT_TRUE(std::filesystem::exists(dir / "INCOMPLETE"));
    std::ifstream rf(dir / "result.json");
    std::string result((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    EXPECT_NE(result.find("\"error\": \""), std::string::npos) << result;
}

TEST(JrTrainStorage, JsonlRowsForFramesAndMessages) {
    Temp temp;
    const auto channels = east(1);
    Recorder r;
    const std::filesystem::path dir = *r.start(temp.p, channels, OutputOptions{false, true, true}).directory;
    FrameEvent ev;
    ev.epoch = 2;
    ev.index = 7;
    ev.frame.bit_offset = 384 * 7;
    ev.frame.sync_matched = true;
    ev.frame.received.fill(0xab);
    r.push_frame(channels[0].id, ev);   // 電文なし: raw だけ
    protocol::Message m;
    m.kind = "Presence";
    m.identity = "123405";
    m.raw_hex = "AB";
    ev.messages.push_back(m);
    ev.corrected_bits = 3;
    r.push_frame(channels[0].id, ev);
    r.push(pcm(channels[0], 0, 0, 1));   // 音声は保存しない設定
    const auto s = r.stop();
    EXPECT_EQ(s.state, "completed") << s.error.value_or("");
    EXPECT_EQ(s.raw_frames, 2u);
    EXPECT_EQ(s.data_messages, 1u);
    EXPECT_EQ(s.files, 0u);
    std::ifstream raw(dir / "raw-frames.jsonl");
    std::string first;
    std::getline(raw, first);
    EXPECT_EQ(first.rfind("{\"bit_offset\":2688,\"channel_id\":\"352537500\",\"corrected_bits\":null,\"epoch\":2,\"format\":\"slotted\",\"frequency_hz\":352537500.0,"
                          "\"index\":7,\"label\":\"D5 音声第1波\",\"marker_errors\":0,\"raw_hex\":\"ABAB",
                          0),
              0u)
        << first;
    std::ifstream msg(dir / "data-messages.jsonl");
    std::string row;
    std::getline(msg, row);
    EXPECT_NE(row.find("\"message\":{\"fields\":[],\"first_bit\":0,\"identity\":\"123405\",\"kind\":\"Presence\",\"last_bit\":0,\"raw_hex\":\"AB\",\"timestamp\":null}"),
              std::string::npos)
        << row;
}

} // namespace

// ---- プリセットと路線(原本 presets.rs / tests/protocol.rs)----
namespace {
using namespace spear::jr_train;

TEST(JrTrainPresets, BuiltinCatalogMatchesOriginal) {
    ASSERT_EQ(presets().size(), 3u);
    const auto* east = find_preset("east");
    ASSERT_NE(east, nullptr);
    EXPECT_EQ(east->channels.size(), 22u);
    EXPECT_EQ(east->channels.front().frequency_hz, 352537500.0);
    EXPECT_EQ(east->channels.back().frequency_hz, 352668750.0);
    EXPECT_EQ(east->center_hz, 352603125.0);
    EXPECT_EQ(east->routes.size(), 112u);
    EXPECT_EQ(east->channels[0].short_label, "D5 V1");
    EXPECT_EQ(east->channels[14].short_label, "COMMON");
    EXPECT_EQ(east->channels[15].short_label, "D1 DATA");
    EXPECT_EQ(find_preset("central")->channels.size(), 3u);
    EXPECT_EQ(find_preset("central")->routes.size(), 2u);
    EXPECT_EQ(find_preset("west")->channels.size(), 19u);
    EXPECT_EQ(find_preset("west")->routes.size(), 19u);
    EXPECT_EQ(find_preset("west")->channels[1].service, Service::Common);   // 原本: 西日本の共用波は 352.54375 MHz
    EXPECT_EQ(channel_config(find_preset("west")->channels[1]).format, protocol::Format::Common);
}

TEST(JrTrainPresets, RoutesSelectVoiceAndDataPlusCommon) {
    const auto* east = find_preset("east");
    // 中央本線(甲府〜小淵沢)D6 は変則割当: 音声 352.61250 / 352.58125、データ 352.66250(原本 ROUTES.ja.md の例)
    const auto r = std::find_if(east->routes.begin(), east->routes.end(), [](const Route& x) { return x.section.find("小淵沢") != std::string::npos && x.designation == "D6"; });
    ASSERT_NE(r, east->routes.end());
    const auto chans = channels_for_routes(*east, {r->id}, true);
    std::vector<double> hz;
    for (const auto& c : chans) hz.push_back(c.frequency_hz);
    EXPECT_EQ(hz, (std::vector<double>{352581250, 352612500, 352625000, 352662500}));
    EXPECT_EQ(channels_for_routes(*east, {r->id}, false).size(), 3u);
    EXPECT_EQ(channels_for_routes(*east, {}, false).size(), 0u);
    // 同じ周波数の路線を重ねても 1 波
    EXPECT_EQ(channels_for_routes(*east, {"east-metro-01", "east-metro-02"}, false).size(), 3u);
}

TEST(JrTrainPresets, ExternalFileIsValidatedAsAWhole) {
    const std::string ok = R"({"schema_version":1,"channel_sets":[{"id":"x","region":"east","direction":"downlink","channels":[
        {"frequency_hz":352600000,"label":"D4 音声第1波","service":"voice"},{"frequency_hz":352606250,"label":"D4 音声第2波","service":"voice"},
        {"frequency_hz":352650000,"label":"D4 データ波","service":"data"}],
        "routes":[{"id":"r1","line":"試験線","section":"甲～乙","designation":"D4","voice_hz":[352600000,352606250],"data_hz":352650000}]}],
        "profiles":[{"id":"mine","label":"試験","channel_set":"x","services":["voice","data"],"capture":{"center_hz":352625000,"sample_rate":240000}}]})";
    const auto p = parse_presets(ok);
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].channels.size(), 3u);
    EXPECT_EQ(p[0].routes.size(), 1u);
    EXPECT_EQ(p[0].channels[2].short_label, "D4 DATA");
    // 壊れたもの: どれも全体を拒否する
    auto bad = [&](const std::string& from, const std::string& to) {
        std::string s = ok;
        s.replace(s.find(from), from.size(), to);
        EXPECT_THROW(parse_presets(s), PresetError) << to;
    };
    bad("\"schema_version\":1", "\"schema_version\":2");
    bad("\"direction\":\"downlink\"", "\"direction\":\"uplink\"");
    bad("\"frequency_hz\":352606250", "\"frequency_hz\":352600000");          // 重複
    bad("\"data_hz\":352650000}", "\"data_hz\":352600000}");                 // データ波でない
    bad("\"voice_hz\":[352600000,352606250]", "\"voice_hz\":[352600000,352600000]");
    bad("\"center_hz\":352625000", "\"center_hz\":351625000");              // 帯域外
    bad("\"services\":[\"voice\",\"data\"]", "\"services\":[\"voice\",\"voice\"]");
    bad("\"label\":\"試験\"", "\"label\":\"試験\",\"extra\":1");              // 未知の項目
    bad("\"id\":\"mine\"", "\"id\":\"Mine\"");
    EXPECT_THROW(parse_presets("{"), PresetError);
    EXPECT_THROW(load_presets_file("/nonexistent/presets.json"), PresetError);
}
} // namespace
