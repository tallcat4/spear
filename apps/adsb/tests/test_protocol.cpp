// Mode S / ADS-B プロトコルの検証。既知ベクトルは Junzi Sun「The 1090MHz Riddle」(https://mode-s.org/decode/)の例題。
#include "protocol/modes.hpp"

#include <gtest/gtest.h>

using namespace spear::adsb;

namespace {
FrameBytes frame(const std::string& hex, int* nbits) {
    FrameBytes b{};
    EXPECT_TRUE(from_hex(hex, b, nbits)) << hex;
    return b;
}
// 先頭 nbits−24 bit に対する CRC を末尾に付ける(xor_addr = AP 形式なら ICAO)
void append_parity(FrameBytes& b, int nbits, uint32_t xor_addr = 0) {
    const int data_bits = nbits - 24;
    FrameBytes z = b;
    for (int i = data_bits; i < nbits; ++i) z[static_cast<std::size_t>(i / 8)] &= static_cast<uint8_t>(~(0x80u >> (i % 8)));
    const uint32_t p = crc_remainder(std::span<const uint8_t>(z.data(), static_cast<std::size_t>(nbits / 8)), nbits) ^ xor_addr;
    b = z;
    for (int i = 0; i < 24; ++i)
        if ((p >> (23 - i)) & 1u) b[static_cast<std::size_t>((data_bits + i) / 8)] |= static_cast<uint8_t>(0x80u >> ((data_bits + i) % 8));
}
}

TEST(AdsbProtocol, CrcOfKnownEsFramesIsZero) {
    for (const char* h : {"8D4840D6202CC371C32CE0576098", "8D40621D58C382D690C8AC2863A7", "8D40621D58C386435CC412692AD6", "8D485020994409940838175B284F"}) {
        int n = 0;
        const auto b = frame(h, &n);
        EXPECT_EQ(n, 112);
        EXPECT_EQ(crc_remainder(b, n), 0u) << h;
    }
}

TEST(AdsbProtocol, HexRoundTrip) {
    int n = 0;
    const auto b = frame("8D4840D6202CC371C32CE0576098", &n);
    EXPECT_EQ(to_hex(b, n), "8D4840D6202CC371C32CE0576098");
    FrameBytes s{};
    EXPECT_TRUE(from_hex("2A00516D492B80", s, &n));
    EXPECT_EQ(n, 56);
    EXPECT_FALSE(from_hex("2A00516D492B8", s, &n));
}

TEST(AdsbProtocol, Identification) {
    int n = 0;
    const auto m = decode(frame("8D4840D6202CC371C32CE0576098", &n), n);
    EXPECT_EQ(m.df, 17);
    EXPECT_TRUE(m.crc_ok);
    EXPECT_EQ(m.icao, 0x4840D6u);
    EXPECT_EQ(m.tc, 4);
    ASSERT_TRUE(m.has_callsign);
    EXPECT_EQ(m.callsign, "KLM1023");
}

TEST(AdsbProtocol, AirbornePositionGlobalAndLocal) {
    int n = 0;
    const auto e = decode(frame("8D40621D58C382D690C8AC2863A7", &n), n);
    const auto o = decode(frame("8D40621D58C386435CC412692AD6", &n), n);
    ASSERT_TRUE(e.crc_ok && o.crc_ok);
    EXPECT_EQ(e.icao, 0x40621Du);
    EXPECT_EQ(e.tc, 11);
    ASSERT_TRUE(e.has_cpr && o.has_cpr);
    EXPECT_FALSE(e.cpr_odd);
    EXPECT_TRUE(o.cpr_odd);
    EXPECT_EQ(e.cpr_lat, 93000u);
    EXPECT_EQ(e.cpr_lon, 51372u);
    EXPECT_EQ(o.cpr_lat, 74158u);
    EXPECT_EQ(o.cpr_lon, 50194u);
    ASSERT_TRUE(e.has_altitude);
    EXPECT_EQ(e.altitude_ft, 38000);
    EXPECT_EQ(e.alt_source, AltSource::Baro);

    Position p;
    ASSERT_TRUE(cpr_global(e.cpr_lat, e.cpr_lon, o.cpr_lat, o.cpr_lon, /*newest_odd=*/false, &p));
    EXPECT_NEAR(p.lat, 52.25720, 1e-4);
    EXPECT_NEAR(p.lon, 3.91937, 1e-4);
    ASSERT_TRUE(cpr_global(e.cpr_lat, e.cpr_lon, o.cpr_lat, o.cpr_lon, /*newest_odd=*/true, &p));
    EXPECT_NEAR(p.lat, 52.26578, 1e-4);
    EXPECT_EQ(cpr_nl(52.25720), 36);
    EXPECT_EQ(cpr_nl(0), 59);
    EXPECT_EQ(cpr_nl(88), 1);

    ASSERT_TRUE(cpr_local(e.cpr_lat, e.cpr_lon, false, Position{52.258, 3.918}, &p));
    EXPECT_NEAR(p.lat, 52.25720, 1e-4);
    EXPECT_NEAR(p.lon, 3.91937, 1e-4);
}

TEST(AdsbProtocol, VelocityGroundSpeedAndAirspeed) {
    int n = 0;
    const auto v = decode(frame("8D485020994409940838175B284F", &n), n);
    ASSERT_TRUE(v.crc_ok);
    EXPECT_EQ(v.tc, 19);
    EXPECT_EQ(v.es_subtype, 1);
    ASSERT_TRUE(v.has_velocity);
    EXPECT_NEAR(v.gs_kt, 159.20, 0.05);
    EXPECT_NEAR(v.track_deg, 182.88, 0.05);
    EXPECT_FALSE(v.track_is_heading);
    ASSERT_TRUE(v.has_vr);
    EXPECT_EQ(v.vr_fpm, -832);

    const auto a = decode(frame("8DA05F219B06B6AF189400CBC33F", &n), n);
    ASSERT_TRUE(a.crc_ok);
    EXPECT_EQ(a.es_subtype, 3);
    ASSERT_TRUE(a.has_velocity);
    EXPECT_TRUE(a.track_is_heading);
    EXPECT_NEAR(a.track_deg, 243.98, 0.05);
    EXPECT_NEAR(a.gs_kt, 375, 0.5);
    ASSERT_TRUE(a.has_vr);
    EXPECT_EQ(a.vr_fpm, -2304);
}

TEST(AdsbProtocol, SquawkFromDf5) {
    int n = 0;
    const auto m = decode(frame("2A00516D492B80", &n), n);
    EXPECT_EQ(m.df, 5);
    EXPECT_TRUE(m.icao_from_ap);
    ASSERT_TRUE(m.has_squawk);
    EXPECT_EQ(m.squawk, 356);   // 0356
}

TEST(AdsbProtocol, Df11AndApFormatsBuiltFromParity) {
    // DF11: CA=5, ICAO=4840D6、パリティ = CRC(II=0)
    FrameBytes b{};
    b[0] = static_cast<uint8_t>((11 << 3) | 5); b[1] = 0x48; b[2] = 0x40; b[3] = 0xD6;
    append_parity(b, 56);
    auto m = decode(b, 56);
    EXPECT_EQ(m.df, 11);
    EXPECT_TRUE(m.crc_ok);
    EXPECT_EQ(m.icao, 0x4840D6u);
    EXPECT_EQ(m.ca, 5);
    // DF4: 高度 38000 ft(Q=1: N = (38000+1000)/25 = 1560。13 bit 配置 C1 A1 C2 A2 C4 A4 M B1 Q B2 D2 B4 D4 に N の 11 bit を置く)、AP = CRC xor ICAO
    const uint32_t nq = 1560;
    const uint32_t ac13 = ((nq & 0x7E0) << 2) | ((nq & 0x10) << 1) | (nq & 0xF) | 0x0010;
    FrameBytes d{};
    d[0] = 4 << 3;
    d[2] = static_cast<uint8_t>((ac13 >> 8) & 0x1F); d[3] = static_cast<uint8_t>(ac13 & 0xFF);
    append_parity(d, 56, 0x4840D6);
    m = decode(d, 56);
    EXPECT_EQ(m.df, 4);
    EXPECT_TRUE(m.icao_from_ap);
    EXPECT_EQ(m.icao, 0x4840D6u);
    ASSERT_TRUE(m.has_altitude);
    EXPECT_EQ(m.altitude_ft, 38000);
}

TEST(AdsbProtocol, SingleBitFix) {
    int n = 0;
    const auto good = frame("8D4840D6202CC371C32CE0576098", &n);
    const auto syn = single_bit_syndromes(112);
    for (int i = 0; i < 112; i += 7) {
        FrameBytes b = good;
        b[static_cast<std::size_t>(i / 8)] ^= static_cast<uint8_t>(0x80u >> (i % 8));
        const uint32_t rem = crc_remainder(b, 112);
        ASSERT_NE(rem, 0u);
        int fixed = -1;
        ASSERT_TRUE(fix_single_bit(b, 112, rem, syn, &fixed));
        EXPECT_EQ(fixed, i);
        EXPECT_EQ(b, good);
    }
}

TEST(AdsbProtocol, GillhamAltitudeSequence) {
    // Q=0 の 12 bit 高度(Mode C Gillham)。dump1090 の移植と同じ並び: C4 のみ = −1200 ft から 100 ft 刻み
    // 13 bit 配置(C1 A1 C2 A2 C4 A4 M B1 Q/D1 B2 D2 B4 D4)→ 12 bit(M を抜く)
    auto ac12_of = [](uint32_t ac13) { return ((ac13 & 0x1F80) >> 1) | (ac13 & 0x3F); };
    bool ok = false;
    EXPECT_EQ(decode_ac12(ac12_of(0x0100), &ok), -1200); EXPECT_TRUE(ok);   // C4
    EXPECT_EQ(decode_ac12(ac12_of(0x0500), &ok), -1100); EXPECT_TRUE(ok);   // C2 C4
    EXPECT_EQ(decode_ac12(ac12_of(0x0400), &ok), -1000); EXPECT_TRUE(ok);   // C2
    EXPECT_EQ(decode_ac12(ac12_of(0x1400), &ok), -900);  EXPECT_TRUE(ok);   // C1 C2
    EXPECT_EQ(decode_ac12(ac12_of(0x1000), &ok), -800);  EXPECT_TRUE(ok);   // C1
    EXPECT_EQ(decode_ac12(ac12_of(0x1002), &ok), -700);  EXPECT_TRUE(ok);   // C1 B4
    decode_ac12(ac12_of(0x0000), &ok); EXPECT_FALSE(ok);                    // C 桁 0 は無効
}

TEST(AdsbProtocol, DistanceAndBearing) {
    const Position tokyo{35.6762, 139.6503}, osaka{34.6937, 135.5023};
    EXPECT_NEAR(distance_km(tokyo, osaka), 392.4, 1);   // 大円距離(R = 6371 km)
    EXPECT_NEAR(bearing_deg(tokyo, osaka), 254, 2);
    EXPECT_NEAR(bearing_deg(tokyo, Position{36.6762, 139.6503}), 0, 1e-6);
    EXPECT_NEAR(bearing_deg(tokyo, Position{35.6762, 140.6503}), 90, 1);
}
