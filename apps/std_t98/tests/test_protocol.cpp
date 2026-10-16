// STD-T98 プロトコルデコーダの C++ 移植が Python 実装と等価であることを検証する。
// golden_vectors.hpp は gen_golden.py が std-t98-tools の Python から生成した真値。
#include "protocol/protocol.hpp"
#include "golden_vectors.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using namespace spear::std_t98;

TEST(StdT98Protocol, MatchesPythonReference) {
    const auto& cases = golden::cases();
    ASSERT_GT(cases.size(), 30u);
    int rich_checked = 0, sacch_checked = 0, pich_checked = 0;
    for (const auto& c : cases) {
        Symbols dw = dewhiten(c.symbols);
        auto fields = parse_frame(dw);

        auto r = decode_rich(fields.rich);
        ASSERT_TRUE(r.valid);
        EXPECT_EQ(r.f, c.rich_f);
        EXPECT_EQ(r.m, c.rich_m);
        EXPECT_EQ(r.d, c.rich_d);
        EXPECT_EQ(r.parity, c.rich_parity);
        EXPECT_EQ(r.parity_ok, c.rich_parity_ok);
        ++rich_checked;

        auto s = decode_sacch(fields.sacch);
        EXPECT_EQ(s.msg_type, c.sacch_msg_type);
        EXPECT_EQ(s.user_code, c.sacch_user);
        EXPECT_EQ(s.maker_code, c.sacch_maker);
        EXPECT_EQ(s.call_stat, c.sacch_call);
        EXPECT_EQ(s.bit_errors, c.sacch_errors);
        EXPECT_EQ(s.crc_ok, c.sacch_crc_ok);
        EXPECT_EQ(s.crc_recv, c.sacch_crc_recv);
        ++sacch_checked;

        auto p = decode_pich(fields.tch1);
        EXPECT_EQ(p.csm, c.pich_csm);
        EXPECT_EQ(p.bit_errors, c.pich_errors);
        EXPECT_EQ(p.crc_ok, c.pich_crc_ok);
        EXPECT_EQ(p.crc_recv, c.pich_crc_recv);
        ++pich_checked;
    }
    EXPECT_EQ(rich_checked, static_cast<int>(cases.size()));
    EXPECT_EQ(sacch_checked, static_cast<int>(cases.size()));
    EXPECT_EQ(pich_checked, static_cast<int>(cases.size()));
}

TEST(StdT98Protocol, QuantizeAndBits) {
    // 量子化とビットマッピングの基本
    auto s = quantize({3.0f, 1.5f, 0.5f, -0.5f, -1.5f, -3.0f});
    EXPECT_EQ(s[0], 3);
    EXPECT_EQ(s[1], 1);
    EXPECT_EQ(s[2], 1);
    EXPECT_EQ(s[3], -1);
    EXPECT_EQ(s[4], -1);
    EXPECT_EQ(s[5], -3);
    // +1→00 +3→01 -1→10 -3→11
    EXPECT_EQ(symbols_to_bits(Symbols{1, 3, -1, -3}), "00011011");
}

TEST(StdT98Protocol, TrafficLlrAgreesWithHardBitsAndConfidence) {
    // 軟判定 LLR の符号は、量子化 → デホワイト → ビット → 9 byte の硬判定と同じビットになる。絶対値は判定境界からの距離
    std::vector<float> raw(192);
    uint32_t r = 12345;
    for (auto& v : raw) {
        r = r * 1103515245u + 12345u;
        const float level = static_cast<float>(static_cast<int>((r >> 16) % 4) * 2 - 3);   // -3 -1 1 3
        v = level + (static_cast<float>((r >> 8) % 100) / 100.f - 0.5f) * 0.8f;              // ±0.4 の揺れ(境界はまたがない)
    }
    const auto payload = traffic_blocks_to_payload(split_traffic_blocks(parse_frame(dewhiten(quantize(raw)))));
    const auto llr = traffic_llr(raw);
    ASSERT_EQ(llr.size(), 288u);
    ASSERT_EQ(payload.size(), 36u);
    for (std::size_t i = 0; i < 288; ++i) {
        const int bit = (payload[i / 8] >> (7 - i % 8)) & 1;
        EXPECT_EQ(llr[i] > 0, bit == 1) << "bit " << i;
        EXPECT_NE(llr[i], 0) << "bit " << i;
    }
    // 外側のレベルほど下位ビット(|y| − 2)の確からしさが大きい。上位ビットは符号そのもの(× kLlrScale)
    std::vector<float> one(192, 1.f);
    one[48] = 3.f;   // TCH1 先頭シンボル(デホワイトの符号に関係なく |y| は同じ)
    const auto l1 = traffic_llr(one);
    EXPECT_EQ(std::abs(l1[0]), 96);    // |−y| × 32
    EXPECT_EQ(l1[1], 32);              // (3 − 2) × 32
    EXPECT_EQ(std::abs(l1[2]), 32);
    EXPECT_EQ(l1[3], -32);             // (1 − 2) × 32
}
