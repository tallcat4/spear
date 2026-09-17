#include "spear/core/block.hpp"

#include <gtest/gtest.h>

using namespace spear;

TEST(BlockPool, AcquireCommitRelease) {
    BlockPool pool(1024 * sizeof(sc16), 4);
    {
        auto b = pool.acquire<sc16>(1024);
        ASSERT_TRUE(b);
        b.data<sc16>()[0] = sc16(1, -1);
        BlockRef r = b.commit(1024);
        EXPECT_FALSE(b);
        EXPECT_EQ(r.header().sample_count, 1024u);
        EXPECT_EQ(r.as<sc16>()[0], sc16(1, -1));
        EXPECT_EQ(pool.stats().in_use, 1u);
        BlockRef r2 = r; // 参照コピー、データはコピーされない
        EXPECT_EQ(r2.as<sc16>().data(), r.as<sc16>().data());
    }
    EXPECT_EQ(pool.stats().in_use, 0u);
    EXPECT_EQ(pool.stats().heap_fallbacks, 0u);
}

TEST(BlockPool, UncommittedBuilderReturnsSlot) {
    BlockPool pool(64, 1);
    { auto b = pool.acquire<uint8_t>(64); (void)b; }
    EXPECT_EQ(pool.stats().in_use, 0u);
}

TEST(BlockPool, HeapFallbackIsCounted) {
    BlockPool pool(64, 1);
    auto a = pool.acquire<uint8_t>(64).commit(64);
    auto b = pool.acquire<uint8_t>(64).commit(64);   // 枯渇 → heap
    auto c = pool.acquire<uint8_t>(4096).commit(4096); // slot より大きい → heap
    EXPECT_EQ(pool.stats().heap_fallbacks, 2u);
    EXPECT_EQ(pool.stats().in_use, 3u);
    EXPECT_EQ(c.bytes().size(), 4096u);
}

TEST(BlockRef, RangeFollowsHeader) {
    BlockPool pool(64, 1);
    auto b = pool.acquire<uint8_t>(10);
    b.header().generation = 3;
    b.header().sample_index = 100;
    auto r = b.commit(10);
    EXPECT_EQ(r.range().generation, 3u);
    EXPECT_EQ(r.range().begin, 100u);
    EXPECT_EQ(r.range().end, 110u);
}
