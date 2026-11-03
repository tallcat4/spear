// 送信の fan-in(§4.1)と TX 不変条件(§4.3): 全 sample を送出するか、underflow event を発行する。
#include "spear/core/core.hpp"
#include "spear/core/synthetic_source.hpp"
#include "spear/core/tx_stream.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace spear;
using namespace std::chrono_literals;

namespace {
// 送信先の代わり: 受け取った sample をそのまま貯める
struct CaptureSink final : Sink {
    std::string name() const override { return "capture"; }
    void write(const Delivery& d) override {
        std::lock_guard lk(mu);
        const auto s = d.block.as<sc16>();
        samples.insert(samples.end(), s.begin(), s.end());
        flags.push_back(d.flags);
    }
    std::vector<sc16> get() { std::lock_guard lk(mu); return samples; }
    std::mutex mu;
    std::vector<sc16> samples;
    std::vector<Flags> flags;
};

BlockRef make_block(BlockPool& pool, uint32_t n, int16_t start, Flags f = {}) {
    auto b = pool.acquire<sc16>(n);
    auto d = b.data<sc16>();
    for (uint32_t i = 0; i < n; ++i) d[i] = sc16(static_cast<int16_t>(start + i), static_cast<int16_t>(-(start + i)));
    b.header().flags = f;
    return b.commit(n);
}

RfConfig tx_cfg() {
    RfConfig c;
    c.direction = Direction::TX;
    c.sample_rate = 1e6;
    c.center_freq = 433e6;
    c.tx_port = RfPort::TrxA;
    return c;
}
} // namespace

TEST(TxStream, WriterWaitsInsteadOfDropping) {
    EventBus ev;
    std::atomic<bool> arm{true};
    BlockPool pool(64 * sizeof(sc16), 8);   // キューの block が指す pool は TxStream より後に壊す(先に宣言)
    TxStream tx({}, &ev, &arm, 2);
    EXPECT_TRUE(tx.try_write(make_block(pool, 8, 0), 10ms));
    EXPECT_TRUE(tx.try_write(make_block(pool, 8, 8), 10ms));
    // 一杯: 捨てずに待ち、空かなければ false(producer が判断する)
    EXPECT_FALSE(tx.try_write(make_block(pool, 8, 16), 20ms));
    EXPECT_EQ(tx.stats().written_blocks, 2u);
    std::thread t([&] { EXPECT_TRUE(tx.write(make_block(pool, 8, 16))); });
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(tx.pop(10ms));
    t.join();
    EXPECT_EQ(tx.stats().written_blocks, 3u);
}

TEST(TxStream, CloseReleasesBlockedWriter) {
    EventBus ev;
    std::atomic<bool> arm{true};
    BlockPool pool(64 * sizeof(sc16), 4);   // キューの block が指す pool は TxStream より後に壊す(先に宣言)
    TxStream tx({}, &ev, &arm, 1);
    ASSERT_TRUE(tx.write(make_block(pool, 8, 0)));
    std::atomic<bool> returned{false}, result{true};
    std::thread t([&] { result = tx.write(make_block(pool, 8, 8)); returned = true; });
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(returned);
    tx.close();
    t.join();
    EXPECT_FALSE(result);
    EXPECT_FALSE(tx.write(make_block(pool, 8, 8)));
}

TEST(TxSource, RejectsReceiveOnlyPort) {
    EventBus ev;
    SyntheticSource src(&ev, {});
    std::atomic<bool> arm{true};
    src.set_tx_arm(&arm);
    RfConfig c = tx_cfg();
    c.tx_port = RfPort::RxA;
    std::string err;
    EXPECT_FALSE(src.configure(c, &err));
    EXPECT_NE(err.find("RXA"), std::string::npos) << err;
}

// 入れた sample がそのまま順に送出される(欠落なし)。burst の始まりと終わりが stats に出る
TEST(TxSource, SendsEverySampleInOrder) {
    EventBus ev;
    SyntheticSource src(&ev, {});
    std::atomic<bool> arm{true};
    src.set_tx_arm(&arm);
    auto sink = std::make_shared<CaptureSink>();
    src.set_tx_sink(sink);
    src.set_realtime(false);
    ASSERT_TRUE(src.configure(tx_cfg()));
    src.start();
    BlockPool pool(1000 * sizeof(sc16), 16);
    auto& tx = src.tx_input();
    int16_t v = 0;
    for (int i = 0; i < 20; ++i, v += 1000) {
        Flags f; if (i == 0) f.set(Flag::StartOfBurst);
        ASSERT_TRUE(tx.write(make_block(pool, 1000, v, f)));
    }
    ASSERT_TRUE(tx.write(make_block(pool, 0, 0, Flag::EndOfBurst)));
    for (int i = 0; i < 200 && tx.stats().sent_samples < 20000; ++i) std::this_thread::sleep_for(5ms);
    std::this_thread::sleep_for(20ms);
    const auto st = tx.stats();
    src.stop();
    EXPECT_EQ(st.sent_samples, 20000u);
    EXPECT_EQ(st.bursts, 1u);
    EXPECT_FALSE(st.in_burst);
    const auto got = sink->get();
    ASSERT_EQ(got.size(), 20000u);
    for (std::size_t i = 0; i < got.size(); ++i) ASSERT_EQ(got[i].real(), static_cast<int16_t>(i)) << i;
    EXPECT_EQ(ev.count(EventKind::Underflow), 0u);
}

// burst の途中で producer が止まると装置は空になる。無言にせず Underflow を出す
TEST(TxSource, StalledProducerMidBurstReportsUnderflow) {
    EventBus ev;
    SyntheticSource src(&ev, {});
    std::atomic<bool> arm{true};
    src.set_tx_arm(&arm);
    src.set_realtime(true);
    ASSERT_TRUE(src.configure(tx_cfg()));
    src.start();
    BlockPool pool(1000 * sizeof(sc16), 8);
    auto& tx = src.tx_input();
    ASSERT_TRUE(tx.write(make_block(pool, 1000, 0, Flag::StartOfBurst)));   // 1 ms 分
    std::this_thread::sleep_for(50ms);                                      // 書き遅れ
    EXPECT_TRUE(tx.stats().in_burst);
    ASSERT_TRUE(tx.write(make_block(pool, 1000, 0, Flag::EndOfBurst)));
    std::this_thread::sleep_for(20ms);
    src.stop();
    ev.flush();
    EXPECT_EQ(tx.stats().underflows, 1u);
    EXPECT_EQ(ev.count(EventKind::Underflow), 1u);
    EXPECT_FALSE(tx.stats().in_burst);
}

// Core::stop_app は Source を先に止める。送信 App が待っている write はそこで解ける
TEST(TxSource, StopUnblocksProducer) {
    EventBus ev;
    SyntheticSource src(&ev, {});
    std::atomic<bool> arm{true};
    src.set_tx_arm(&arm);
    src.set_realtime(true);
    RfConfig c = tx_cfg();
    c.sample_rate = 200e3;
    ASSERT_TRUE(src.configure(c));
    src.start();
    BlockPool pool(16384 * sizeof(sc16), 64);
    auto& tx = src.tx_input();
    std::atomic<bool> done{false};
    std::thread producer([&] {
        while (tx.write(make_block(pool, 16384, 0))) {}
        done = true;
    });
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(done);
    src.stop();
    producer.join();
    EXPECT_TRUE(done);
}

TEST(TxSource, TxAttenIsDeclaredImmediately) {
    EventBus ev;
    SyntheticSource src(&ev, {});
    std::atomic<bool> arm{true};
    src.set_tx_arm(&arm);
    ASSERT_TRUE(src.configure(tx_cfg()));
    const auto v0 = src.state_version();
    EXPECT_DOUBLE_EQ(src.config().tx_atten, kTxAttenFullRangeDb);   // 既定は最小出力
    EXPECT_TRUE(src.set_tx_atten(12.5));
    EXPECT_DOUBLE_EQ(src.config().tx_atten, 12.5);
    EXPECT_GT(src.state_version(), v0);
}

// Core::run_app は App を始めてから Source を始める。実機の open は数秒かかるので、その間 App の write は待ち続けなければならない
// (2026-11-04 実機: 1 s で諦めていたため最初の 32 block だけ送られて underflow になった)
TEST(TxSource, ProducerStartedBeforeSourceWaitsForIt) {
    EventBus ev;
    SyntheticSource src(&ev, {});
    std::atomic<bool> arm{true};
    src.set_tx_arm(&arm);
    auto sink = std::make_shared<CaptureSink>();
    src.set_tx_sink(sink);
    src.set_realtime(false);
    ASSERT_TRUE(src.configure(tx_cfg()));
    BlockPool pool(1000 * sizeof(sc16), 8);
    auto& tx = src.tx_input();
    std::thread producer([&] {
        for (int i = 0; i < 3 * static_cast<int>(kDefaultTxQueueBlocks); ++i)
            ASSERT_TRUE(tx.write(make_block(pool, 1000, 0, i == 0 ? Flags(Flag::StartOfBurst) : Flags())));
        ASSERT_TRUE(tx.write(make_block(pool, 0, 0, Flag::EndOfBurst)));
    });
    std::this_thread::sleep_for(1500ms);   // 装置の open の代わり
    src.start();
    producer.join();
    for (int i = 0; i < 200 && tx.stats().in_burst; ++i) std::this_thread::sleep_for(5ms);
    std::this_thread::sleep_for(20ms);
    src.stop();
    EXPECT_EQ(sink->get().size(), 3 * kDefaultTxQueueBlocks * 1000);
}

// TX MASTER が SAFE の間は、どの producer の送信も Core の入口で拒否される(装置には何も渡らない)
TEST(TxMaster, SafeRefusesEveryWrite) {
    Core core(".");
    auto src = std::make_unique<SyntheticSource>(&core.events(), SyntheticSignal{});
    auto* raw = src.get();
    auto sink = std::make_shared<CaptureSink>();
    raw->set_tx_sink(sink);
    core.set_source(std::move(src));
    EXPECT_FALSE(core.tx_armed());   // 起動時は SAFE
    ASSERT_TRUE(raw->configure(tx_cfg()));
    raw->start();
    BlockPool pool(1000 * sizeof(sc16), 8);
    EXPECT_FALSE(core.tx().write(make_block(pool, 1000, 0, Flag::StartOfBurst)));
    EXPECT_FALSE(core.tx().write(make_block(pool, 1000, 0)));
    std::this_thread::sleep_for(30ms);
    raw->stop();
    core.events().flush();
    EXPECT_EQ(core.tx().stats().sent_samples, 0u);
    EXPECT_EQ(core.tx().stats().bursts, 0u);
    EXPECT_TRUE(sink->get().empty());
    // 拒否は無言ではない(拒否が始まったときに 1 回)
    int refused = 0;
    for (const auto& e : core.events().history()) if (e.detail.find("TX MASTER is SAFE") != std::string::npos) ++refused;
    EXPECT_EQ(refused, 1);
}

// 送信中に SAFE にすると、キューに残った分は送らずに burst が閉じる
TEST(TxMaster, DisarmMidBurstEndsBurst) {
    Core core(".");
    auto src = std::make_unique<SyntheticSource>(&core.events(), SyntheticSignal{});
    auto* raw = src.get();
    auto sink = std::make_shared<CaptureSink>();
    raw->set_tx_sink(sink);
    raw->set_realtime(true);
    core.set_source(std::move(src));
    core.set_tx_armed(true);
    RfConfig c = tx_cfg();
    c.sample_rate = 200e3;
    ASSERT_TRUE(raw->configure(c));
    raw->start();
    BlockPool pool(16384 * sizeof(sc16), 64);
    std::atomic<int> written{0};
    std::thread producer([&] {
        bool first = true;
        while (core.tx().write(make_block(pool, 16384, 0, first ? Flags(Flag::StartOfBurst) : Flags()))) { first = false; ++written; }
    });
    std::this_thread::sleep_for(150ms);
    EXPECT_TRUE(core.tx().stats().in_burst);
    core.set_tx_armed(false);
    producer.join();   // 次の write が拒否されて抜ける
    std::this_thread::sleep_for(50ms);
    const auto st = core.tx().stats();
    EXPECT_FALSE(st.in_burst);
    EXPECT_LT(st.sent_samples, static_cast<uint64_t>(written.load()) * 16384u);   // 残りは送っていない
    const uint64_t at_safe = sink->get().size();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(sink->get().size(), at_safe);
    raw->stop();
}
