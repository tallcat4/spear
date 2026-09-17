#include "modes.hpp"

#include <cmath>
#include <cstdio>
#include <numbers>

namespace spear::adsb {

namespace {
constexpr double kPi = std::numbers::pi;
double cprmod(double a, double b) { return a - b * std::floor(a / b); }
}

int frame_bits_for_df(int df) { return df >= 16 ? 112 : 56; }

uint32_t crc_remainder(std::span<const uint8_t> bytes, int nbits) {
    // 生成多項式 x^24 + ... で全ビット(パリティ込み)を割った剰余。DF17 なら 0
    uint32_t rem = 0;
    for (int i = 0; i < nbits; ++i) {
        const uint32_t bit = (bytes[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1u;
        rem = (rem << 1) | bit;
        if (rem & 0x1000000u) rem ^= 0x1000000u | kCrcPoly;
    }
    return rem & 0xFFFFFFu;
}

std::array<uint32_t, 112> single_bit_syndromes(int nbits) {
    std::array<uint32_t, 112> s{};
    for (int i = 0; i < nbits; ++i) {
        FrameBytes b{};
        b[static_cast<std::size_t>(i / 8)] = static_cast<uint8_t>(0x80u >> (i % 8));
        s[static_cast<std::size_t>(i)] = crc_remainder(b, nbits);
    }
    return s;
}

bool fix_single_bit(FrameBytes& b, int nbits, uint32_t remainder, const std::array<uint32_t, 112>& syndromes, int* fixed_bit) {
    if (remainder == 0) return false;
    for (int i = 0; i < nbits; ++i) {
        if (syndromes[static_cast<std::size_t>(i)] != remainder) continue;
        b[static_cast<std::size_t>(i / 8)] ^= static_cast<uint8_t>(0x80u >> (i % 8));
        if (fixed_bit) *fixed_bit = i;
        return true;
    }
    return false;
}

uint32_t bits(const FrameBytes& b, int pos, int n) {
    uint32_t v = 0;
    for (int i = pos - 1; i < pos - 1 + n; ++i) v = (v << 1) | ((b[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1u);
    return v;
}

// ---- 高度 / スコーク(dump1090 の decodeID13Field / ModeAToModeC の移植)----
namespace {
// 13 bit フィールド(C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4)→ 16 進 Gillham 配置(A<<12 | B<<8 | C<<4 | D、各桁 = bit1 + 2·bit2 + 4·bit4)
uint32_t id13_to_gillham(uint32_t f) {
    uint32_t g = 0;
    if (f & 0x1000) g |= 0x0010;   // C1
    if (f & 0x0800) g |= 0x1000;   // A1
    if (f & 0x0400) g |= 0x0020;   // C2
    if (f & 0x0200) g |= 0x2000;   // A2
    if (f & 0x0100) g |= 0x0040;   // C4
    if (f & 0x0080) g |= 0x4000;   // A4
    //  0x0040 = X / M(スコークでは未使用)
    if (f & 0x0020) g |= 0x0100;   // B1
    if (f & 0x0010) g |= 0x0001;   // D1 / Q
    if (f & 0x0008) g |= 0x0200;   // B2
    if (f & 0x0004) g |= 0x0002;   // D2
    if (f & 0x0002) g |= 0x0400;   // B4
    if (f & 0x0001) g |= 0x0004;   // D4
    return g;
}
// Gillham(Mode C)→ 100 ft 単位。無効なら false
bool gillham_to_100ft(uint32_t g, int* out) {
    if ((g & 0xFFFF888Bu) || (g & 0x000000F0u) == 0) return false;   // 未使用 bit が立っている / C 桁が 0
    int hundreds = 0, five_hundreds = 0;
    if (g & 0x0010) hundreds ^= 0x007;   // C1
    if (g & 0x0020) hundreds ^= 0x003;   // C2
    if (g & 0x0040) hundreds ^= 0x001;   // C4
    if ((hundreds & 5) == 5) hundreds ^= 2;   // 7 → 5
    if (hundreds > 5) return false;
    if (g & 0x0002) five_hundreds ^= 0x0FF;   // D2
    if (g & 0x0004) five_hundreds ^= 0x07F;   // D4
    if (g & 0x1000) five_hundreds ^= 0x03F;   // A1
    if (g & 0x2000) five_hundreds ^= 0x01F;   // A2
    if (g & 0x4000) five_hundreds ^= 0x00F;   // A4
    if (g & 0x0100) five_hundreds ^= 0x007;   // B1
    if (g & 0x0200) five_hundreds ^= 0x003;   // B2
    if (g & 0x0400) five_hundreds ^= 0x001;   // B4
    if (five_hundreds & 1) hundreds = 6 - hundreds;
    const int n = five_hundreds * 5 + hundreds - 13;
    if (n < -12) return false;
    *out = n;
    return true;
}
}

int decode_ac13(uint32_t ac13, bool* valid) {
    const bool m_bit = ac13 & 0x0040, q_bit = ac13 & 0x0010;
    if (m_bit) { *valid = false; return 0; }   // メートル単位(実運用でほぼ無い)
    if (q_bit) {
        const uint32_t n = ((ac13 & 0x1F80) >> 2) | ((ac13 & 0x0020) >> 1) | (ac13 & 0x000F);
        *valid = true;
        return static_cast<int>(n) * 25 - 1000;
    }
    int n100 = 0;
    *valid = gillham_to_100ft(id13_to_gillham(ac13), &n100);
    return n100 * 100;
}

int decode_ac12(uint32_t ac12, bool* valid) {
    if (ac12 & 0x10) {   // Q
        const uint32_t n = ((ac12 & 0x0FE0) >> 1) | (ac12 & 0x000F);
        *valid = true;
        return static_cast<int>(n) * 25 - 1000;
    }
    // M=0 を bit 6 に挿して 13 bit Gillham にする
    const uint32_t ac13 = ((ac12 & 0x0FC0) << 1) | (ac12 & 0x003F);
    int n100 = 0;
    *valid = gillham_to_100ft(id13_to_gillham(ac13), &n100);
    return n100 * 100;
}

int decode_id13(uint32_t id13) {
    const uint32_t g = id13_to_gillham(id13);
    return static_cast<int>(((g >> 12) & 7) * 1000 + ((g >> 8) & 7) * 100 + ((g >> 4) & 7) * 10 + (g & 7));
}

std::string decode_callsign(const FrameBytes& b) {
    static constexpr char kCharset[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
    std::string s;
    for (int i = 0; i < 8; ++i) s.push_back(kCharset[bits(b, 41 + 6 * i, 6)]);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

namespace {
void decode_es(const FrameBytes& b, Message& m) {
    m.tc = static_cast<int>(bits(b, 33, 5));
    if (m.tc >= 1 && m.tc <= 4) {
        m.category = static_cast<int>(bits(b, 38, 3));
        m.callsign = decode_callsign(b);
        m.has_callsign = true;
    } else if (m.tc >= 5 && m.tc <= 8) {
        m.on_ground = true;   // 地上位置(CPR の地上形式は未対応)
    } else if ((m.tc >= 9 && m.tc <= 18) || (m.tc >= 20 && m.tc <= 22)) {
        bool ok = false;
        const int alt = decode_ac12(bits(b, 41, 12), &ok);
        if (ok) { m.has_altitude = true; m.altitude_ft = alt; m.alt_source = m.tc >= 20 ? AltSource::Gnss : AltSource::Baro; }
        m.has_cpr = true;
        m.cpr_odd = bits(b, 54, 1) != 0;
        m.cpr_lat = bits(b, 55, 17);
        m.cpr_lon = bits(b, 72, 17);
    } else if (m.tc == 19) {
        m.es_subtype = static_cast<int>(bits(b, 38, 3));
        if (m.es_subtype == 1 || m.es_subtype == 2) {
            const bool s_ew = bits(b, 46, 1), s_ns = bits(b, 57, 1);
            const uint32_t v_ew = bits(b, 47, 10), v_ns = bits(b, 58, 10);
            if (v_ew != 0 && v_ns != 0) {
                const double k = m.es_subtype == 2 ? 4.0 : 1.0;
                const double vx = (static_cast<double>(v_ew) - 1) * k * (s_ew ? -1 : 1);   // 東が正
                const double vy = (static_cast<double>(v_ns) - 1) * k * (s_ns ? -1 : 1);   // 北が正
                m.has_velocity = true;
                m.gs_kt = std::hypot(vx, vy);
                m.track_deg = std::atan2(vx, vy) * 180.0 / kPi;
                if (m.track_deg < 0) m.track_deg += 360.0;
            }
        } else if (m.es_subtype == 3 || m.es_subtype == 4) {
            const bool hdg_ok = bits(b, 46, 1);
            const uint32_t as = bits(b, 58, 10);
            if (hdg_ok && as != 0) {
                m.has_velocity = true;
                m.track_is_heading = true;
                m.track_deg = bits(b, 47, 10) * 360.0 / 1024.0;
                m.gs_kt = (static_cast<double>(as) - 1) * (m.es_subtype == 4 ? 4.0 : 1.0);   // 対気速度(IAS/TAS)
            }
        }
        const uint32_t vr = bits(b, 70, 9);
        if (vr != 0) { m.has_vr = true; m.vr_fpm = (static_cast<int>(vr) - 1) * 64 * (bits(b, 69, 1) ? -1 : 1); }
    } else if (m.tc == 28) {
        m.es_subtype = static_cast<int>(bits(b, 38, 3));
        if (m.es_subtype == 1) { m.has_squawk = true; m.squawk = decode_id13(bits(b, 44, 13)); }
    }
}
}

Message decode(const FrameBytes& b, int nbits) {
    Message m;
    m.df = df_of(b);
    m.remainder = crc_remainder(std::span<const uint8_t>(b.data(), static_cast<std::size_t>(nbits / 8)), nbits);
    switch (m.df) {
    case 11:
        m.ca = static_cast<int>(bits(b, 6, 3));
        m.icao = bits(b, 9, 24);
        m.crc_ok = m.remainder < 80;   // 下位は II(0–15)/ SI(16–79)
        break;
    case 17:
    case 18:
        m.ca = static_cast<int>(bits(b, 6, 3));
        m.icao = bits(b, 9, 24);
        m.crc_ok = m.remainder == 0;
        // DF18 は CF 0/1/6 のとき ME が ADS-B 形式(dump1090 と同じ扱い)
        if (m.crc_ok && (m.df == 17 || m.ca == 0 || m.ca == 1 || m.ca == 6)) decode_es(b, m);
        break;
    case 0: case 4: case 16: case 20: {
        m.icao = m.remainder; m.icao_from_ap = true;
        bool ok = false;
        const int alt = decode_ac13(bits(b, 20, 13), &ok);
        if (ok) { m.has_altitude = true; m.altitude_ft = alt; m.alt_source = AltSource::Baro; }
        if (m.df == 0 || m.df == 16) m.on_ground = bits(b, 6, 1) != 0;              // VS
        else { const auto fs = bits(b, 6, 3); m.on_ground = fs == 1 || fs == 3; }   // FS
        break;
    }
    case 5: case 21: {
        m.icao = m.remainder; m.icao_from_ap = true;
        m.has_squawk = true; m.squawk = decode_id13(bits(b, 20, 13));
        const auto fs = bits(b, 6, 3); m.on_ground = fs == 1 || fs == 3;
        break;
    }
    default:
        break;
    }
    return m;
}

// ---- CPR ----
int cpr_nl(double lat) {
    if (lat == 0) return 59;
    const double a = std::fabs(lat);
    if (a == 87) return 2;
    if (a > 87) return 1;
    constexpr double nz = 15;
    const double c = std::cos(kPi / 180.0 * lat);
    return static_cast<int>(std::floor(2 * kPi / std::acos(1 - (1 - std::cos(kPi / (2 * nz))) / (c * c))));
}

bool cpr_global(uint32_t even_lat, uint32_t even_lon, uint32_t odd_lat, uint32_t odd_lon, bool newest_odd, Position* out) {
    constexpr double dlat_e = 360.0 / 60, dlat_o = 360.0 / 59;
    const double le = even_lat / 131072.0, lo = odd_lat / 131072.0, ge = even_lon / 131072.0, go = odd_lon / 131072.0;
    const double j = std::floor(59 * le - 60 * lo + 0.5);
    double lat_e = dlat_e * (cprmod(j, 60) + le);
    double lat_o = dlat_o * (cprmod(j, 59) + lo);
    if (lat_e >= 270) lat_e -= 360;
    if (lat_o >= 270) lat_o -= 360;
    if (lat_e < -90 || lat_e > 90 || lat_o < -90 || lat_o > 90) return false;
    if (cpr_nl(lat_e) != cpr_nl(lat_o)) return false;   // ゾーン境界をまたいだペア
    const double lat = newest_odd ? lat_o : lat_e;
    const int nl = cpr_nl(lat);
    const int ni = std::max(newest_odd ? nl - 1 : nl, 1);
    const double m = std::floor(ge * (nl - 1) - go * nl + 0.5);
    double lon = (360.0 / ni) * (cprmod(m, ni) + (newest_odd ? go : ge));
    if (lon >= 180) lon -= 360;
    *out = {lat, lon};
    return true;
}

bool cpr_local(uint32_t cpr_lat, uint32_t cpr_lon, bool odd, Position ref, Position* out) {
    const double la = cpr_lat / 131072.0, lo = cpr_lon / 131072.0;
    const double dlat = 360.0 / (odd ? 59 : 60);
    const double j = std::floor(ref.lat / dlat) + std::floor(0.5 + cprmod(ref.lat, dlat) / dlat - la);
    const double lat = dlat * (j + la);
    if (lat < -90 || lat > 90) return false;
    const int nl = cpr_nl(lat) - (odd ? 1 : 0);
    const double dlon = nl > 0 ? 360.0 / nl : 360.0;
    const double m = std::floor(ref.lon / dlon) + std::floor(0.5 + cprmod(ref.lon, dlon) / dlon - lo);
    double lon = dlon * (m + lo);
    if (lon >= 180) lon -= 360;
    if (lon < -180) lon += 360;
    *out = {lat, lon};
    return true;
}

double distance_km(Position a, Position b) {
    constexpr double r = 6371.0;
    const double p1 = a.lat * kPi / 180, p2 = b.lat * kPi / 180;
    const double dp = (b.lat - a.lat) * kPi / 180, dl = (b.lon - a.lon) * kPi / 180;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * r * std::asin(std::sqrt(h));
}

double bearing_deg(Position from, Position to) {
    const double p1 = from.lat * kPi / 180, p2 = to.lat * kPi / 180, dl = (to.lon - from.lon) * kPi / 180;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double b = std::atan2(y, x) * 180 / kPi;
    if (b < 0) b += 360;
    return b;
}

std::string to_hex(const FrameBytes& b, int nbits) {
    std::string s;
    char buf[4];
    for (int i = 0; i < nbits / 8; ++i) { std::snprintf(buf, sizeof buf, "%02X", b[static_cast<std::size_t>(i)]); s += buf; }
    return s;
}

bool from_hex(const std::string& hex, FrameBytes& b, int* nbits) {
    if (hex.size() != 14 && hex.size() != 28) return false;
    b.fill(0);
    for (std::size_t i = 0; i < hex.size(); ++i) {
        const char c = hex[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        b[i / 2] = static_cast<uint8_t>((b[i / 2] << 4) | v);
    }
    *nbits = static_cast<int>(hex.size() * 4);
    return true;
}

} // namespace spear::adsb
