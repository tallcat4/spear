// STD-T98 プロトコルデコーダの C++ 移植が Python 実装と等価であることを検証する。
// golden_vectors.hpp は gen_golden.py が std-t98-tools の Python から生成した真値。
#include "protocol/protocol.hpp"
#include "golden_vectors.hpp"

#include <gtest/gtest.h>

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
