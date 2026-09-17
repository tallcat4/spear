#include "map_data.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

// SPEAR_ADSB_MAP_DIR は CMake が apps/adsb/map の絶対パスで定義する(std_t98/secret/models.cpp と同じ .incbin)
#define SPEAR_STR2(x) #x
#define SPEAR_STR(x) SPEAR_STR2(x)
asm(".section .rodata\n"
    ".balign 64\n"
    ".global spear_adsb_map_data\nspear_adsb_map_data:\n"
    ".incbin \"" SPEAR_STR(SPEAR_ADSB_MAP_DIR) "/world.spearmap\"\n"
    ".global spear_adsb_map_end\nspear_adsb_map_end:\n"
    ".byte 0\n"
    ".previous\n");
extern "C" const uint8_t spear_adsb_map_data[], spear_adsb_map_end[];

namespace spear::adsb::map {

std::span<const uint8_t> embedded_blob() { return {spear_adsb_map_data, static_cast<std::size_t>(spear_adsb_map_end - spear_adsb_map_data)}; }

namespace {
struct Reader {
    std::span<const uint8_t> b;
    std::size_t pos = 0;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (pos + sizeof(T) > b.size()) { ok = false; return v; }
        std::memcpy(&v, b.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    std::string str() {
        const uint8_t n = get<uint8_t>();
        if (!ok || pos + n > b.size()) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(b.data() + pos), n);
        pos += n;
        return s;
    }
    bool lines(std::vector<Polyline>& out) {
        const uint32_t n = get<uint32_t>();
        if (!ok || n > 10'000'000) return false;
        out.resize(n);
        for (auto& pl : out) {
            const uint32_t m = get<uint32_t>();
            if (!ok || m > 10'000'000 || pos + static_cast<std::size_t>(m) * 8 > b.size()) return false;
            pl.pts.resize(m);
            std::memcpy(pl.pts.data(), b.data() + pos, static_cast<std::size_t>(m) * 8);
            pos += static_cast<std::size_t>(m) * 8;
            pl.lon_min = pl.lat_min = 1e9f; pl.lon_max = pl.lat_max = -1e9f;
            for (const auto& p : pl.pts) {
                pl.lon_min = std::min(pl.lon_min, p.lon); pl.lon_max = std::max(pl.lon_max, p.lon);
                pl.lat_min = std::min(pl.lat_min, p.lat); pl.lat_max = std::max(pl.lat_max, p.lat);
            }
        }
        return true;
    }
};
}

bool parse(std::span<const uint8_t> blob, MapData& out, std::string* err) {
    auto fail = [&](const char* why) { if (err) *err = why; out = MapData{}; return false; };
    if (blob.size() < 8 || std::memcmp(blob.data(), "SPMAP1\0\0", 8) != 0) return fail("bad magic");
    Reader r{blob, 8};
    if (!r.lines(out.land) || !r.lines(out.lakes) || !r.lines(out.borders) || !r.lines(out.admin1)) return fail("bad polyline section");
    const uint32_t np = r.get<uint32_t>();
    if (!r.ok || np > 1'000'000) return fail("bad places");
    out.places.resize(np);
    for (auto& p : out.places) { p.p.lon = r.get<float>(); p.p.lat = r.get<float>(); p.pop = r.get<uint32_t>(); p.rank = r.get<uint8_t>(); p.name = r.str(); }
    const uint32_t na = r.get<uint32_t>();
    if (!r.ok || na > 1'000'000) return fail("bad airports");
    out.airports.resize(na);
    for (auto& a : out.airports) { a.p.lon = r.get<float>(); a.p.lat = r.get<float>(); a.kind = r.get<uint8_t>(); a.ident = r.str(); a.name = r.str(); }
    const uint32_t nr = r.get<uint32_t>();
    if (!r.ok || nr > 1'000'000) return fail("bad runways");
    out.runways.resize(nr);
    for (auto& w : out.runways) { w.a.lon = r.get<float>(); w.a.lat = r.get<float>(); w.b.lon = r.get<float>(); w.b.lat = r.get<float>(); w.length_m = r.get<float>(); }
    if (!r.ok) return fail("truncated");
    return true;
}

const MapData& world() {
    static MapData data;
    static std::once_flag once;
    std::call_once(once, [] { parse(embedded_blob(), data); });
    return data;
}

} // namespace spear::adsb::map
