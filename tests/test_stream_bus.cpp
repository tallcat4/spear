// Stream Bus のテスト (要件 §12.3)
//   * consumer 追加・削除時に producer 側コードが変わらないこと
//   * Lossless consumer の drop 数がゼロであること
//   * Latest Only consumer の遅延が Lossless 側に影響しないこと
#include "spear/core/stream.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace spear;
using namespace std::chrono_literals;

namespace {

struct Fixture {
    EventBus events;
    BlockPool pool{4096 * sizeof(sc16), 512};
    Stream<sc16> stream{StreamMeta{"test.rx", DataType::ComplexInt16, 1e6, 0, 1e6, "FS", Direction::RX}, &events};
    uint64_t seq = 0, idx = 0;

    // producer: consumer の存在を一切知らない
    void produce(uint32_t n = 4096, uint64_t gen = 0) {
        auto b = pool.acquire<sc16>(n);
        b.header().generation = gen;
        b.header().sequence = seq++;
        b.header().sample_index = idx;
        idx += n;
        stream.publish(b.commit(n));
    }
};

} // namespace

TEST(StreamBus, PublishWithoutConsumersIsFine) {
    Fixture f;
    for (int i = 0; i < 10; ++i) f.produce();
    EXPECT_EQ(f.stream.published_blocks(), 10u);
    EXPECT_EQ(f.pool.stats().in_use, 0u);
}

TEST(StreamBus, ThreeConsumersShareOneBlockWithoutCopy) {
    Fixture f;
    auto a = f.stream.subscribe("spectrum", DeliveryPolicy::LatestOnly, 2);
    auto b = f.stream.subscribe("waterfall", DeliveryPolicy::LatestOnly, 2);
    auto c = f.stream.subscribe("recorder", DeliveryPolicy::Lossless, 64);
    f.produce();
    auto da = a->try_pop(); auto db = b->try_pop(); auto dc = c->try_pop();
    ASSERT_TRUE(da && db && dc);
    EXPECT_EQ(da->block.as<sc16>().data(), db->block.as<sc16>().data());
    EXPECT_EQ(db->block.as<sc16>().data(), dc->block.as<sc16>().data());
    EXPECT_EQ(f.pool.stats().in_use, 1u);
    da.reset(); db.reset(); dc.reset();
    EXPECT_EQ(f.pool.stats().in_use, 0u);
}

TEST(StreamBus, LosslessConsumerThatKeepsUpDropsNothing) {
    Fixture f;
    // producer は全速で publish するので、キュー容量は総 block 数以上にして「読み遅れ」を作らない
    auto c = f.stream.subscribe("recorder", DeliveryPolicy::Lossless, 4096);
    std::atomic<uint64_t> got{0};
    std::thread consumer([&] {
        while (!c->eos()) {
            if (auto d = c->pop(100ms)) {
                EXPECT_FALSE(d->flags.has(Flag::Discontinuity));
                got += d->block.header().sample_count;
            }
        }
    });
    for (int i = 0; i < 2000; ++i) f.produce();
    f.stream.end();
    consumer.join();
    auto st = c->stats();
    EXPECT_EQ(st.dropped_blocks, 0u);
    EXPECT_EQ(st.dropped_samples, 0u);
    EXPECT_EQ(got.load(), 2000ull * 4096);
    EXPECT_EQ(f.events.count(EventKind::ConsumerOverflow), 0u);
}

TEST(StreamBus, LosslessOverflowIsNeverSilent) {
    Fixture f;
    auto c = f.stream.subscribe("decoder", DeliveryPolicy::Lossless, 4);
    // consumer が読まないまま 10 block → 6 block 溢れる
    for (int i = 0; i < 10; ++i) f.produce();
    auto st = c->stats();
    EXPECT_EQ(st.dropped_blocks, 6u);
    EXPECT_EQ(st.dropped_samples, 6u * 4096);
    EXPECT_EQ(st.overflow_bursts, 1u);
    f.events.flush();
    EXPECT_EQ(f.events.count(EventKind::ConsumerOverflow), 1u);
    // 溢れ中は Discontinuity event 未確定。4 block 読んでから次を publish すると確定する。
    for (int i = 0; i < 4; ++i) { auto d = c->try_pop(); ASSERT_TRUE(d); EXPECT_FALSE(d->flags.has(Flag::Discontinuity)); }
    f.produce();
    auto d = c->try_pop();
    ASSERT_TRUE(d);
    EXPECT_TRUE(d->flags.has(Flag::Discontinuity)) << "溢れ後の最初の block に discontinuity が立つ";
    EXPECT_FALSE(d->block.header().flags.has(Flag::Discontinuity)) << "block 自体(他 consumer から見える)は汚さない";
    f.events.flush();
    EXPECT_EQ(f.events.count(EventKind::Discontinuity), 1u);
    auto hist = f.events.history();
    ASSERT_FALSE(hist.empty());
    EXPECT_EQ(hist.front().kind, EventKind::Discontinuity);
    EXPECT_EQ(hist.front().range.begin, 4ull * 4096);
    EXPECT_EQ(hist.front().range.end, 10ull * 4096);
    EXPECT_EQ(hist.front().value, 6 * 4096);
}

TEST(StreamBus, LatestOnlyDropsOldAndCounts) {
    Fixture f;
    auto v = f.stream.subscribe("waterfall", DeliveryPolicy::LatestOnly, 1);
    for (int i = 0; i < 100; ++i) f.produce();
    auto d = v->try_pop();
    ASSERT_TRUE(d);
    EXPECT_EQ(d->block.header().sequence, 99u);
    EXPECT_TRUE(d->flags.has(Flag::Discontinuity));
    EXPECT_EQ(v->stats().dropped_blocks, 99u);
    EXPECT_FALSE(v->try_pop());
}

TEST(StreamBus, StuckLatestOnlyDoesNotAffectLossless) {
    Fixture f;
    auto stuck = f.stream.subscribe("gui", DeliveryPolicy::LatestOnly, 1); // 一度も pop しない
    // 深さ 5000: 5000 ブロックを全速で publish しても consumer スレッドのスケジューリング次第で溢れないように
    // (このテストの主題は「詰まった LatestOnly が Lossless に影響しない」ことで、Lossless の queue 溢れではない)
    auto rec   = f.stream.subscribe("recorder", DeliveryPolicy::Lossless, 5000);
    std::atomic<uint64_t> got{0};
    std::thread consumer([&] {
        while (!rec->eos())
            if (auto d = rec->pop(100ms)) got += d->block.header().sample_count;
    });
    using clock = std::chrono::steady_clock;
    std::chrono::nanoseconds worst{0};
    for (int i = 0; i < 5000; ++i) {
        const auto t0 = clock::now();
        f.produce();
        worst = std::max(worst, std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0));
    }
    f.stream.end();
    consumer.join();
    EXPECT_EQ(rec->stats().dropped_blocks, 0u);
    EXPECT_EQ(got.load(), 5000ull * 4096);
    EXPECT_EQ(stuck->stats().dropped_blocks, 4999u);
    EXPECT_LT(worst, 5ms) << "publish() は consumer の状態に関わらず短時間で返る";
}

TEST(StreamBus, DetachByDroppingHandleProducerUnchanged) {
    Fixture f;
    auto a = f.stream.subscribe("a", DeliveryPolicy::Lossless, 8);
    {
        auto tmp = f.stream.subscribe("tmp", DeliveryPolicy::LatestOnly, 1);
        f.produce();
        EXPECT_EQ(f.stream.consumer_count(), 2u);
    }
    f.produce(); // 同じ producer コード
    EXPECT_EQ(f.stream.consumer_count(), 1u);
    EXPECT_EQ(a->stats().queue_depth, 2u);
    EXPECT_EQ(f.stream.stats().size(), 1u);
}

TEST(StreamBus, EndOfStreamReachesConsumers) {
    Fixture f;
    auto a = f.stream.subscribe("a", DeliveryPolicy::Lossless, 8);
    f.produce();
    f.stream.end();
    EXPECT_FALSE(a->eos()) << "キューが残っている間は eos ではない";
    EXPECT_TRUE(a->try_pop());
    EXPECT_TRUE(a->eos());
    auto late = f.stream.subscribe("late", DeliveryPolicy::Lossless, 8);
    EXPECT_TRUE(late->eos());
}

TEST(StreamBus, ConcurrentSubscribeWhilePublishing) {
    // TSan 用: publish 中の subscribe / detach が競合しない
    Fixture f;
    std::atomic<bool> stop{false};
    std::thread producer([&] { while (!stop) f.produce(); });
    for (int i = 0; i < 200; ++i) {
        auto s = f.stream.subscribe("s" + std::to_string(i), DeliveryPolicy::LatestOnly, 2);
        s->pop(1ms);
    }
    stop = true;
    producer.join();
}

TEST(ContinuityChecker, DetectsGapsWithinGenerationOnly) {
    ContinuityChecker c;
    BlockHeader h;
    h.generation = 0; h.sample_index = 0; h.sample_count = 100;
    EXPECT_FALSE(c.check(h));
    h.sample_index = 100;
    EXPECT_FALSE(c.check(h));
    h.sample_index = 250;
    auto g = c.check(h);
    ASSERT_TRUE(g);
    EXPECT_EQ(g->expected, 200u);
    EXPECT_EQ(g->actual, 250u);
    h.generation = 1; h.sample_index = 0;
    EXPECT_FALSE(c.check(h)) << "generation が変われば比較しない";
}
