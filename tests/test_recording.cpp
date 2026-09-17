// M1: B210LiveSource と RecordingSource が同一 API で扱え、保存 IQ を同じ DSP へ入力できる。
// Acceptance 12 (provenance): sample index 範囲から IQ を切り出し再入力して同一結果が得られる。
#include "spear/core/recording.hpp"
#include "spear/core/synthetic_source.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

using namespace spear;
using namespace std::chrono_literals;

namespace {

std::string tmp_base(const char* n) {
    auto p = std::filesystem::temp_directory_path() / ("spear_test_" + std::string(n));
    std::filesystem::remove(sigmf::data_path(p.string()));
    std::filesystem::remove(sigmf::meta_path(p.string()));
    std::filesystem::remove(sigmf::sidecar_path(p.string()));
    return p.string();
}

// Source → Lossless consumer → Sink。「同じ App コード」に相当する消費ループ。
template <class F>
void drain(Source& src, F&& on_delivery) {
    auto sub = src.output().subscribe("app", DeliveryPolicy::Lossless, 64);
    src.start();
    while (!sub->eos()) if (auto d = sub->pop(500ms)) on_delivery(*d);
    src.stop();
    EXPECT_EQ(sub->stats().dropped_blocks, 0u);
}

} // namespace

TEST(Recording, RoundTripThroughSigmf) {
    EventBus ev;
    SyntheticSignal sig;
    sig.tones = {{100e3, 0.3}, {-300e3, 0.2}};
    sig.noise_amplitude = 0.01;
    sig.max_samples = 4096 * 10;
    RfConfig cfg; cfg.sample_rate = 2e6; cfg.center_freq = 145e6;

    const auto base = tmp_base("roundtrip");
    std::vector<sc16> original;
    {
        SyntheticSource src(&ev, sig, 4096);
        ASSERT_TRUE(src.configure(cfg));
        src.set_realtime(false);
        SigmfRecorder rec(base, src.output().meta(), cfg, &ev);
        ASSERT_TRUE(rec.ok());
        drain(src, [&](const Delivery& d) {
            rec.write(d);
            auto s = d.block.as<sc16>();
            original.insert(original.end(), s.begin(), s.end());
        });
        rec.close();
        EXPECT_EQ(rec.samples_written(), sig.max_samples);
        EXPECT_EQ(rec.meta().captures.size(), 1u);
        EXPECT_DOUBLE_EQ(rec.meta().captures[0].frequency, 145e6);
        EXPECT_TRUE(rec.meta().discontinuities.empty());
    }
    {
        RecordingSource src(&ev, base, false, 4096);
        ASSERT_TRUE(src.open());
        EXPECT_DOUBLE_EQ(src.config().sample_rate, 2e6);
        EXPECT_DOUBLE_EQ(src.config().center_freq, 145e6);
        EXPECT_EQ(src.output().meta().id, std::string(kRadioRxStreamId)); // 実機と同じ stream
        src.set_realtime(false);
        std::vector<sc16> replayed;
        drain(src, [&](const Delivery& d) {
            auto s = d.block.as<sc16>();
            replayed.insert(replayed.end(), s.begin(), s.end());
        });
        ASSERT_EQ(replayed.size(), original.size());
        EXPECT_EQ(std::memcmp(replayed.data(), original.data(), original.size() * sizeof(sc16)), 0);
    }
}

TEST(Recording, LoopAdvancesGeneration) {
    EventBus ev;
    SyntheticSignal sig; sig.max_samples = 1024 * 3;
    RfConfig cfg; cfg.sample_rate = 1e6;
    const auto base = tmp_base("loop");
    {
        SyntheticSource src(&ev, sig, 1024);
        ASSERT_TRUE(src.configure(cfg));
        src.set_realtime(false);
        SigmfRecorder rec(base, src.output().meta(), cfg, &ev);
        drain(src, [&](const Delivery& d) { rec.write(d); });
        rec.close();
    }
    RecordingSource src(&ev, base, true, 1024);
    ASSERT_TRUE(src.open());
    src.set_realtime(false);
    auto sub = src.output().subscribe("app", DeliveryPolicy::Lossless, 64);
    src.start();
    uint64_t max_gen = 0; int blocks = 0; bool saw_disc = false;
    ContinuityChecker c;
    while (blocks < 8) {
        if (auto d = sub->pop(500ms)) {
            ++blocks;
            max_gen = std::max(max_gen, d->block.header().generation);
            if (d->flags.has(Flag::Discontinuity)) saw_disc = true;
            EXPECT_FALSE(c.check(d->block.header())) << "generation 内では連続";
        }
    }
    src.stop();
    EXPECT_GE(max_gen, 2u);
    EXPECT_TRUE(saw_disc);
}

TEST(Recording, DiscontinuityIsRecordedInSidecar) {
    EventBus ev;
    RfConfig cfg; cfg.sample_rate = 1e6;
    const auto base = tmp_base("disc");
    BlockPool pool(1024 * sizeof(sc16), 8);
    StreamMeta sm{"radio.rx", DataType::ComplexInt16, 1e6, 0, 1e6, "FS", Direction::RX};
    SigmfRecorder rec(base, sm, cfg, &ev);
    auto mk = [&](uint64_t idx, Flags f = {}) {
        auto b = pool.acquire<sc16>(1024);
        b.header().sample_index = idx;
        b.header().flags = f;
        return Delivery{b.commit(1024), f};
    };
    rec.write(mk(0));
    rec.write(mk(1024));
    rec.write(mk(4096, Flag::Overflow | Flag::Discontinuity)); // 2048 sample 欠落
    rec.close();
    ASSERT_EQ(rec.meta().discontinuities.size(), 1u);
    EXPECT_EQ(rec.meta().discontinuities[0].file_sample, 2048u);
    EXPECT_EQ(rec.meta().discontinuities[0].missing_samples, 2048u);
    EXPECT_NE(rec.meta().discontinuities[0].flags.find("overflow"), std::string::npos);
    EXPECT_EQ(rec.meta().captures.size(), 2u);
    EXPECT_EQ(rec.meta().captures[1].sample_index, 4096u);

    sigmf::Meta m;
    ASSERT_TRUE(sigmf::read(base, m));
    EXPECT_EQ(m.total_samples, 3072u);
    ASSERT_EQ(m.captures.size(), 2u);
    EXPECT_EQ(m.captures[1].sample_start, 2048u);
    EXPECT_EQ(m.captures[1].sample_index, 4096u);
}

TEST(Recording, ProvenanceCutAndReplayMatches) {
    // Acceptance 12 の骨格: "decoded frame の sample index 範囲" から IQ を逆算して切り出す。
    EventBus ev;
    SyntheticSignal sig; sig.tones = {{50e3, 0.4}}; sig.max_samples = 2048 * 4;
    RfConfig cfg; cfg.sample_rate = 1e6;
    const auto base = tmp_base("prov");
    std::vector<sc16> original;
    SyntheticSource src(&ev, sig, 2048);
    ASSERT_TRUE(src.configure(cfg));
    src.set_realtime(false);
    {
        SigmfRecorder rec(base, src.output().meta(), cfg, &ev);
        drain(src, [&](const Delivery& d) {
            rec.write(d);
            auto s = d.block.as<sc16>();
            original.insert(original.end(), s.begin(), s.end());
        });
        rec.close();
    }
    // "frame" が generation 0 の sample [3000, 5000) から得られたとする
    const SampleRange frame{0, 3000, 5000};
    sigmf::Meta m;
    ASSERT_TRUE(sigmf::read(base, m));
    // capture から file offset を逆算
    uint64_t file_off = 0; bool found = false;
    for (const auto& c : m.captures)
        if (c.generation == frame.generation && c.sample_index <= frame.begin) { file_off = c.sample_start + (frame.begin - c.sample_index); found = true; }
    ASSERT_TRUE(found);
    std::ifstream f(sigmf::data_path(base), std::ios::binary);
    f.seekg(static_cast<std::streamoff>(file_off * sizeof(sc16)));
    std::vector<sc16> cut(frame.end - frame.begin);
    f.read(reinterpret_cast<char*>(cut.data()), static_cast<std::streamsize>(cut.size() * sizeof(sc16)));
    ASSERT_TRUE(f);
    EXPECT_EQ(std::memcmp(cut.data(), original.data() + frame.begin, cut.size() * sizeof(sc16)), 0);
}
