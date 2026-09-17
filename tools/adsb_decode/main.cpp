// spear-adsb-decode — SigMF 録音を ADS-B Receiver に流す headless 解析 (§11, §12.3 golden set)
//   spear-adsb-decode <base> [--lo-offset Hz] [--cutoff Hz] [--taps N] [--ratio dB] [--no-fix] [--ref lat,lon] [--golden out.txt] [--quiet]
// 出力: 受理フレームの行(index DF ICAO hex 解読内容)、最後に統計と航空機一覧。
// --golden f: 受理フレームを "index hex" で 1 行ずつ書く(apps/adsb/tests の golden、CRC OK の DF17 は自己検証的)。
// LO オフセットは録音の中心周波数から求める(1090 MHz − center)。
#include "aircraft.hpp"
#include "receiver.hpp"
#include "spear/core/sigmf.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace spear;
using namespace spear::adsb;

namespace {
const char* arg(int argc, char** argv, const char* key, const char* def) {
    for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return def;
}
bool flag(int argc, char** argv, const char* key) {
    for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], key)) return true;
    return false;
}
std::string describe(const Message& m) {
    char buf[256];
    std::string s;
    if (m.has_callsign) s += " id=" + m.callsign;
    if (m.has_squawk) { std::snprintf(buf, sizeof buf, " sq=%04d", m.squawk); s += buf; }
    if (m.has_altitude) { std::snprintf(buf, sizeof buf, " alt=%d%s", m.altitude_ft, m.alt_source == AltSource::Gnss ? "g" : ""); s += buf; }
    if (m.has_cpr) { std::snprintf(buf, sizeof buf, " cpr%s=%u,%u", m.cpr_odd ? "o" : "e", m.cpr_lat, m.cpr_lon); s += buf; }
    if (m.has_velocity) { std::snprintf(buf, sizeof buf, " %s=%.0fkt %s=%.1f", m.track_is_heading ? "as" : "gs", m.gs_kt, m.track_is_heading ? "hdg" : "trk", m.track_deg); s += buf; }
    if (m.has_vr) { std::snprintf(buf, sizeof buf, " vr=%+d", m.vr_fpm); s += buf; }
    if (m.on_ground) s += " gnd";
    return s;
}
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: spear-adsb-decode <sigmf base> [--lo-offset Hz] [--cutoff Hz] [--taps N] [--ratio dB] [--no-fix] [--ref lat,lon] [--golden out.txt] [--quiet]"); return 1; }
    const std::string base = argv[1];
    sigmf::Meta meta;
    std::string err;
    if (!sigmf::read(base, meta, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const double center = meta.captures.empty() ? 0.0 : meta.captures[0].frequency;
    ReceiverConfig cfg;
    cfg.in_rate = meta.sample_rate;
    cfg.lo_offset_hz = std::stod(arg(argc, argv, "--lo-offset", std::to_string(1090e6 - center).c_str()));
    cfg.lowpass_cutoff_hz = std::stod(arg(argc, argv, "--cutoff", "1.5e6"));
    cfg.lowpass_taps = std::stoi(arg(argc, argv, "--taps", "47"));
    cfg.preamble_ratio_db = std::stod(arg(argc, argv, "--ratio", "6"));
    cfg.fix_single_bit = !flag(argc, argv, "--no-fix");
    const bool quiet = flag(argc, argv, "--quiet");
    AircraftTable table;
    if (const char* r = arg(argc, argv, "--ref", nullptr)) {
        double lat = 0, lon = 0;
        if (std::sscanf(r, "%lf,%lf", &lat, &lon) == 2) { table.set_reference(Position{lat, lon}); }
    }
    std::FILE* golden = arg(argc, argv, "--golden", nullptr) ? std::fopen(arg(argc, argv, "--golden", ""), "w") : nullptr;
    std::printf("rate=%.0f spc=%d center=%.6f MHz lo_offset=%+.0f Hz cutoff=%.0f taps=%d ratio=%.1f dB fix=%d\n", cfg.in_rate,
                std::max(1, static_cast<int>(cfg.in_rate / 2e6 + 0.5)), center / 1e6, cfg.lo_offset_hz, cfg.lowpass_cutoff_hz, cfg.lowpass_taps, cfg.preamble_ratio_db, cfg.fix_single_bit);

    Receiver rx(cfg);
    Observer obs;
    obs.frame = [&](const Frame& f, std::span<const float>) {
        const Aircraft& a = table.update(f);
        if (golden) std::fprintf(golden, "%llu %s\n", (unsigned long long)f.input_sample_index, to_hex(f.bytes, f.nbits).c_str());
        if (quiet) return;
        std::printf("%10llu t=%8.3f DF%-2d %06X %s%s rssi=%.1f snr=%.1f%s", (unsigned long long)f.input_sample_index, f.t_s, f.msg.df, f.msg.icao,
                    to_hex(f.bytes, f.nbits).c_str(), f.fixed ? " FIX" : "", f.rssi_db, f.snr_db, describe(f.msg).c_str());
        if (f.msg.has_cpr && a.position && a.last_position_s == f.t_s) {
            std::printf(" pos=%.5f,%.5f", a.position->lat, a.position->lon);
            if (a.distance_km) std::printf(" %.1fkm/%.0f", *a.distance_km, *a.bearing_deg);
        }
        std::printf("\n");
    };
    rx.set_observer(obs);

    std::ifstream f(sigmf::data_path(base), std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", sigmf::data_path(base).c_str()); return 1; }
    const std::size_t block = static_cast<std::size_t>(std::atoi(arg(argc, argv, "--block", "65536")));
    const bool cf = meta.dtype == DataType::ComplexFloat32;
    std::vector<sc16> raw(block);
    std::vector<cf32> iq(block);
    uint64_t idx = 0;
    while (true) {
        std::size_t n = 0;
        if (cf) { f.read(reinterpret_cast<char*>(iq.data()), static_cast<std::streamsize>(block * sizeof(cf32))); n = static_cast<std::size_t>(f.gcount()) / sizeof(cf32); }
        else {
            f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(block * sizeof(sc16)));
            n = static_cast<std::size_t>(f.gcount()) / sizeof(sc16);
            for (std::size_t i = 0; i < n; ++i) iq[i] = cf32(raw[i].real() / 32768.f, raw[i].imag() / 32768.f);
        }
        if (n == 0) break;
        rx.process(std::span<const cf32>(iq.data(), n), idx);
        idx += n;
    }
    if (golden) std::fclose(golden);
    const auto& m = rx.metrics();
    std::printf("\nsamples=%llu (%.1f s) preambles=%llu frames=%llu crc_bad=%llu fixed=%llu ap_unknown=%llu\n", (unsigned long long)idx, static_cast<double>(idx) / cfg.in_rate,
                (unsigned long long)m.preambles, (unsigned long long)m.frames, (unsigned long long)m.crc_bad, (unsigned long long)m.fixed, (unsigned long long)m.ap_unknown);
    std::printf("by DF:");
    for (int d = 0; d < 32; ++d) if (m.by_df[d]) std::printf(" DF%d=%llu", d, (unsigned long long)m.by_df[d]);
    std::printf("\npositions=%llu rejected=%llu\n\n", (unsigned long long)table.total_positions(), (unsigned long long)table.rejected_positions());
    std::printf("%-6s %-8s %-4s %6s %5s %5s %6s %-7s %-7s %6s %5s %6s\n", "ICAO", "CALLSIGN", "SQ", "ALT", "GS", "TRK", "VR", "LAT", "LON", "DIST", "MSGS", "RSSI");
    for (const auto& a : table.all()) {
        std::printf("%06X %-8s %4s %6s %5s %5s %6s %-8s %-8s %6s %5llu %6.1f\n", a.icao, a.callsign.c_str(),
                    a.squawk ? std::to_string(*a.squawk).c_str() : "-",
                    a.altitude_ft ? std::to_string(*a.altitude_ft).c_str() : "-",
                    a.gs_kt ? std::to_string(static_cast<int>(*a.gs_kt)).c_str() : "-",
                    a.track_deg ? std::to_string(static_cast<int>(*a.track_deg)).c_str() : "-",
                    a.vr_fpm ? std::to_string(*a.vr_fpm).c_str() : "-",
                    a.position ? std::to_string(a.position->lat).substr(0, 8).c_str() : "-",
                    a.position ? std::to_string(a.position->lon).substr(0, 8).c_str() : "-",
                    a.distance_km ? (std::to_string(*a.distance_km).substr(0, 5) + "k").c_str() : "-",
                    (unsigned long long)a.messages, a.rssi_db);
    }
    return 0;
}
