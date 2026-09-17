// Mode S / ADS-B(1090 MHz Extended Squitter)プロトコルデコーダ — 純 C++(Qt / SDR 非依存、決定的、テスト可能)
//
// 一次資料: ICAO Annex 10 Vol. IV(Mode S)、RTCA DO-260B(1090ES)。ビット位置の記法は
// Junzi Sun「The 1090MHz Riddle」(https://mode-s.org/decode/)に合わせる(1 始まり、MSB から)。
// 参照実装: dump1090 / readsb(GPL)。高度(Gillham)とスコークのビット配置は dump1090 の decodeID13Field / ModeAToModeC を移植。
//
// フレーム: 56 bit(DF 0/4/5/11)または 112 bit(DF 16–21、24)。末尾 24 bit はパリティ:
//   DF 11/17/18: PI(= CRC そのもの、全呼出応答は II/SI が下位 7 bit に載る)→ 剰余 0(DF11 は < 80)で CRC OK
//   DF 0/4/5/16/20/21: AP(= CRC xor ICAO)→ 剰余が ICAO アドレスになる(既知アドレスとの照合は呼び出し側)
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace spear::adsb {

inline constexpr uint32_t kCrcPoly = 0xFFF409;   // x^24 + x^23 + x^22 + x^21 + x^20 + x^19 + x^18 + x^17 + x^16 + x^15 + x^14 + x^13 + x^12 + x^10 + x^3 + 1

// フレーム(最大 112 bit = 14 byte)。短フレームは先頭 7 byte だけ使う
using FrameBytes = std::array<uint8_t, 14>;

// DF 値からフレーム長(bit)。DF ≥ 16 は 112、それ以外は 56
int frame_bits_for_df(int df);
inline int df_of(const FrameBytes& b) { return b[0] >> 3; }

// 全ビットに対する CRC-24 剰余。パリティ込みで計算するので DF17 なら 0、AP 形式なら ICAO アドレスになる
uint32_t crc_remainder(std::span<const uint8_t> bytes, int nbits);
// 1 bit 誤りのシンドローム表(bit i だけが立ったフレームの剰余)。nbits = 56 / 112。fix_single_bit が使う
std::array<uint32_t, 112> single_bit_syndromes(int nbits);
// 剰余がどれかのシンドロームに一致すればその bit を反転して true(DF17/18 のように剰余 0 が正しいフレーム用)
bool fix_single_bit(FrameBytes& b, int nbits, uint32_t remainder, const std::array<uint32_t, 112>& syndromes, int* fixed_bit = nullptr);

// bit 位置 pos(1 始まり)から n bit を取り出す(Riddle の記法そのまま)
uint32_t bits(const FrameBytes& b, int pos, int n);

// ---- 解読結果(1 フレーム) ----
enum class AltSource : uint8_t { None, Baro, Gnss };

struct Message {
    int      df = -1;
    int      ca = 0;             // DF11/17: capability、DF18: CF
    uint32_t icao = 0;           // DF11/17/18: AA フィールド、AP 形式: 剰余(呼び出し側で既知アドレスと照合)
    bool     icao_from_ap = false;
    uint32_t remainder = 0;      // CRC 剰余
    bool     crc_ok = false;     // DF11/17/18 の判定。AP 形式は常に false(照合してから信用する)
    int      tc = -1;            // DF17/18 の ME type code
    int      es_subtype = 0;

    bool     has_callsign = false; std::string callsign; int category = 0;
    bool     has_altitude = false; int altitude_ft = 0; AltSource alt_source = AltSource::None;
    bool     has_squawk = false; int squawk = 0;                 // 4 桁 8 進(表示は %04o)
    bool     has_cpr = false; bool cpr_odd = false; uint32_t cpr_lat = 0, cpr_lon = 0;   // 17 bit 生値(空中位置)
    bool     has_velocity = false; double gs_kt = 0, track_deg = 0; bool track_is_heading = false;
    bool     has_vr = false; int vr_fpm = 0;
    bool     on_ground = false;  // DF17 TC 5–8(地上位置。位置は未対応、フラグのみ)
};

// フレームを解読する。CRC 判定と AP 形式の剰余 → ICAO 抽出まで行う。既知 ICAO との照合は Receiver / 呼び出し側
Message decode(const FrameBytes& b, int nbits);

// ---- 部品(テスト用に公開) ----
int decode_ac13(uint32_t ac13, bool* valid);    // DF0/4/16/20 の 13 bit 高度 → ft(M=1 のメートル単位は非対応 → valid=false)
int decode_ac12(uint32_t ac12, bool* valid);    // DF17 の 12 bit 高度 → ft
int decode_id13(uint32_t id13);                 // DF5/21 の 13 bit ID → スコーク(8 進 4 桁を 10 進で表した値、例 0x0EE → 0356)
std::string decode_callsign(const FrameBytes& b);   // TC1–4 の 8 文字(末尾の空白は除く)

// ---- CPR(Compact Position Reporting、空中位置)----
struct Position { double lat = 0, lon = 0; };
int cpr_nl(double lat);   // 経度ゾーン数 NL(lat)
// 偶奇ペアからの大域解。newest_odd = 新しい方が奇。失敗(NL 不一致等)なら false
bool cpr_global(uint32_t even_lat, uint32_t even_lon, uint32_t odd_lat, uint32_t odd_lon, bool newest_odd, Position* out);
// 基準位置(前回位置か地上局)からの局所解。基準から 1/2 ゾーン以内であること
bool cpr_local(uint32_t cpr_lat, uint32_t cpr_lon, bool odd, Position ref, Position* out);

// 距離 [km] と方位 [deg、真北 0、時計回り]
double distance_km(Position a, Position b);
double bearing_deg(Position from, Position to);

std::string to_hex(const FrameBytes& b, int nbits);
bool from_hex(const std::string& hex, FrameBytes& b, int* nbits);

} // namespace spear::adsb
