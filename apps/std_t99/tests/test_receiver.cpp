// STD-T99 Annex 2 受信機の回帰テスト。std-t99-receiver-sharp の tools/synthetic-check.py と src/T99.Smoke の判定を移したもの。
//   * 規格書 7.7.2 の試験信号(4 送信 / 29 フレーム)を IQ にして Monitor(Front → Receiver → Session → CallMonitor → 音声)に通す。
//     原本は SDR# の IQ レート(37.5〜250 kHz)で試していた。SPEAR では App の 4 Msps(LO から 250 kHz)と、Front を素通しにした 100 kHz
//   * 80 ms 格子に対する開始位置 20 通り、雑音だけ 60 s でフレームが出ないこと
//   * 選択呼出の着信判定(表 Annex 2-4-14、受信接続試験 表 Annex 2-7-6)、チャネル表
// 実録音の golden はまだ無い(実機の受信は未確認。docs/apps/std_t99.md)。
#include "channels.hpp"
#include "monitor.hpp"
#include "spec_iq.hpp"
#include "voice/tokuder.hpp"

#include <gtest/gtest.h>

#include <random>

using namespace spear;
using namespace spear::std_t99;

namespace {

struct Captured {
    struct F { bool crc, voice, new_call; CallInfo call; };
    std::vector<F> frames;
    std::vector<std::string> json;
    std::vector<float> audio;
    MonitorStatus status;
};

// IQ を不揃いなブロックで流す(原本の試験と同じく 100〜4000 sample)。音声はその場で復号する(VoiceWorker の代わり)
Captured run(const std::vector<cf32>& iq, double rate, double shift_hz, std::size_t lead = 0) {
    Captured c;
    tokuder::Decoder dec(tokuder::model());
    std::vector<float> pcm;
    MonitorObserver o;
    std::unique_ptr<Monitor> m;
    o.frame = [&](const FrameEvent& e) {
        c.frames.push_back({e.frame.crc_ok(), e.frame.voice(), e.new_call, *m->calls().current()});
        c.json.push_back(e.json);
    };
    o.voice = [&](const VoiceCommand& v) {
        pcm.clear();
        if (v.kind == VoiceCommand::Kind::Flush) dec.flush(pcm); else dec.push(v.info.data(), pcm);
        c.audio.insert(c.audio.end(), pcm.begin(), pcm.end());
    };
    m = std::make_unique<Monitor>(rate, shift_hz, o);
    std::vector<cf32> input;
    if (lead) {   // 80 ms 格子に対してフレームの位置をずらす
        std::mt19937 r(static_cast<uint32_t>(lead));
        std::normal_distribution<float> g(0.f, 0.03f);
        for (std::size_t i = 0; i < lead; ++i) input.emplace_back(g(r), g(r));
    }
    input.insert(input.end(), iq.begin(), iq.end());
    std::mt19937 rng(3);
    std::uniform_int_distribution<std::size_t> len(100, 4000);
    const std::size_t scale = static_cast<std::size_t>(rate / kReceiverRate);
    std::size_t pos = 0;
    while (pos < input.size()) {
        const std::size_t n = std::min(len(rng) * scale, input.size() - pos);
        m->process(std::span<const cf32>(input.data() + pos, n), pos);
        pos += n;
    }
    m->flush_voice();
    c.status = m->status();
    return c;
}

void check_spec(const Captured& c, const std::string& where) {
    SCOPED_TRACE(where);
    int crc = 0, voice = 0;
    std::vector<std::size_t> calls;
    for (std::size_t i = 0; i < c.frames.size(); ++i) {
        crc += c.frames[i].crc;
        voice += c.frames[i].voice;
        if (c.frames[i].new_call) calls.push_back(i);
    }
    ASSERT_EQ(c.frames.size(), 29u);
    EXPECT_EQ(crc, 29);
    ASSERT_EQ(calls.size(), 4u) << "同期バーストの数";
    // 各送信の最後のフレームの時点の呼の情報
    std::vector<CallInfo> last;
    for (std::size_t k = 0; k < calls.size(); ++k) last.push_back(c.frames[(k + 1 < calls.size() ? calls[k + 1] : c.frames.size()) - 1].call);
    for (const auto& l : last) {
        EXPECT_EQ(l.caller.value_or(""), "127-0123456");
        ASSERT_TRUE(l.position.has_value());
        EXPECT_EQ(l.position->latitude, 35402565u);
        EXPECT_EQ(l.position->longitude, 139450235u);
        EXPECT_EQ(l.position->altitude_m, 7);
        EXPECT_TRUE(l.position->valid);
    }
    EXPECT_EQ(last[0].comm_type, 4) << "1-1 個別通話";
    EXPECT_EQ(last[0].called.value_or(""), "127-0000654");
    EXPECT_TRUE(last[0].ended);
    ASSERT_TRUE(last[0].gps_time.has_value());
    EXPECT_EQ(last[0].gps_time->time, 235120u);
    EXPECT_EQ(last[0].gps_time->date, 260718u);
    EXPECT_EQ(last[0].gps_time->speed_01kn, 27);
    EXPECT_EQ(last[0].gps_time->direction_deg, 267);
    EXPECT_EQ(last[1].name, "無線試験No.9");
    EXPECT_EQ(last[3].name, "無線試験No.9");
    EXPECT_EQ(last[2].call_control, 1) << "2-1 緊急通信 1";
    EXPECT_TRUE(last[2].emergency());
    EXPECT_EQ(last[3].comm_type, 1) << "6-1 グループ通話";
    EXPECT_EQ(last[3].group, 3);
    // TOKUDER の音声: 音声フレームのスロットすべて(1031 Hz の試験音は音声ではないので、あることだけを見る)
    EXPECT_EQ(c.audio.size(), static_cast<std::size_t>(voice) * 640);
    float peak = 0;
    for (float v : c.audio) { ASSERT_TRUE(std::isfinite(v)); peak = std::max(peak, std::abs(v)); }
    EXPECT_GT(peak, 0.01f);
    for (const auto& j : c.json) {
        EXPECT_NE(j.find("\"burst\":null"), std::string::npos);
        EXPECT_NE(j.find("\"raw\""), std::string::npos);
    }
}

} // namespace

TEST(StdT99Spec, VectorsDecodeWithValidCrc) {
    // 規格書 7.7.2 の 53 フレーム: シンボルのまま復号して、すべて CRC が通り RICH が妥当
    std::size_t n = 0;
    for (const auto& v : spec::vectors()) {
        std::vector<float> raw(v.symbols.begin(), v.symbols.end());
        const auto f = decode_frame(raw);
        EXPECT_TRUE(f.crc_ok()) << v.test << " frame " << v.frame;
        EXPECT_TRUE(f.plausible()) << v.test << " frame " << v.frame;
        ++n;
    }
    EXPECT_EQ(n, 53u);
}

TEST(StdT99Spec, ReceivesAt4MspsWithLoOffset) {
    // App と同じ: 4 Msps、チャネルは LO から +250 kHz、搬送波のずれ +180 Hz
    const double rate = 4e6, lo_offset = 250e3;
    const auto iq = spec::spec_iq(rate, lo_offset + 180);
    const auto c = run(iq, rate, lo_offset);
    check_spec(c, "4 Msps");
    EXPECT_TRUE(c.status.have_offset);
    // 表示用のずれ(同期中の弁別器出力の平均)は原本のままで、真値の 6〜7 割に出る(100 kHz でも同じ。docs/apps/std_t99.md「既知の問題」)。
    // 復号は各フレームが同期語の DC を差し引くので影響しない。ここでは符号だけを見る
    EXPECT_GT(c.status.freq_offset_hz, 30);
}

TEST(StdT99Spec, ReceivesAt100kHzWithCarrierOffsets) {
    for (double offset : {-600.0, -350.0, 0.0, 420.0}) {
        const auto c = run(spec::spec_iq(kReceiverRate, offset), kReceiverRate, 0);
        check_spec(c, "100 kHz offset " + std::to_string(offset));
    }
}

TEST(StdT99Spec, EveryStartOffsetOnTheFrameGrid) {
    // 開始位置を 80 ms 格子に対して 20 通りずらしても、全フレームと音声フレーム分の音声(20 × 640)が出る
    const auto iq = spec::spec_iq(kReceiverRate, 180);
    for (std::size_t lead = 0; lead < 8000; lead += 400) {
        const auto c = run(iq, kReceiverRate, 0, lead);
        int crc = 0, voice = 0;
        for (const auto& f : c.frames) { crc += f.crc; voice += f.voice; }
        EXPECT_EQ(c.frames.size(), 29u) << "lead " << lead;
        EXPECT_EQ(crc, 29) << "lead " << lead;
        EXPECT_EQ(voice, 20) << "lead " << lead;
        EXPECT_EQ(c.audio.size(), 20u * 640u) << "lead " << lead;
    }
}

TEST(StdT99Spec, NoFramesFromNoise) {
    std::mt19937 rng(11);
    std::normal_distribution<float> g(0.f, 0.1f);
    std::vector<cf32> noise(static_cast<std::size_t>(60 * kReceiverRate));
    for (auto& x : noise) x = {g(rng), g(rng)};
    const auto c = run(noise, kReceiverRate, 0);
    EXPECT_EQ(c.frames.size(), 0u);
}

TEST(StdT99Call, SelectiveCallTable) {
    // 表 Annex 2-4-14 と受信接続試験(表 Annex 2-7-6)。std-t99-receiver-sharp src/T99.Smoke と同じ組み合わせ
    auto individual = [](std::string called) { CallInfo c; c.comm_type = 4; c.called = std::move(called); c.group = 0; return c; };
    auto group = [](int g) { CallInfo c; c.comm_type = 1; c.called = "000-0000000"; c.group = g; return c; };
    const std::string me = "127-0000654";
    EXPECT_EQ(accepts(individual(me), Standby::Individual, me, 0), true);
    EXPECT_EQ(accepts(individual("127-0000001"), Standby::Individual, me, 0), false);
    EXPECT_EQ(accepts(individual(me), Standby::Group, me, 3), true) << "グループ待受も自局宛ての個別通話は受ける(注 1)";
    EXPECT_EQ(accepts(group(3), Standby::Individual, me, 3), false) << "個別待受はグループ・一斉を受けない(注 2)";
    EXPECT_EQ(accepts(group(0), Standby::Individual, me, 0), false);
    EXPECT_EQ(accepts(group(3), Standby::Group, me, 3), true);
    EXPECT_EQ(accepts(group(3), Standby::Group, me, 5), false);
    EXPECT_EQ(accepts(group(3), Standby::Group, me, 0), true) << "グループ番号 0 はすべてのグループ";
    EXPECT_EQ(accepts(group(0), Standby::Group, me, 5), true) << "一斉通話";
    EXPECT_EQ(accepts(CallInfo{}, Standby::Group, me, 3), std::nullopt) << "通話種別が未確定";
    EXPECT_EQ(accepts(group(9), Standby::Monitor, "", 0), true);
    EXPECT_EQ(normalize_device_id(127, 654), "127-0000654");
    EXPECT_EQ(normalize_device_id(128, 1), std::nullopt);
    EXPECT_EQ(normalize_device_id(0, 2097152), std::nullopt);
}

TEST(StdT99Call, ChannelTable) {
    EXPECT_EQ(find_channel(146984375), 18);
    EXPECT_EQ(find_channel(142934375), 1);
    EXPECT_EQ(find_channel(142984375), 9);
    EXPECT_EQ(find_channel(146934375), 10);
    EXPECT_EQ(find_channel(146000000), std::nullopt);
    EXPECT_DOUBLE_EQ(channel_freq_hz(kCallingChannel), 146984375.0);
}
