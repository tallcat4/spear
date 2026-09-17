#include "aircraft.hpp"

#include <algorithm>
#include <cmath>

namespace spear::adsb {

const Aircraft* AircraftTable::find(uint32_t icao) const {
    auto it = std::find_if(list_.begin(), list_.end(), [icao](const Aircraft& a) { return a.icao == icao; });
    return it == list_.end() ? nullptr : &*it;
}

Aircraft& AircraftTable::get_or_create(uint32_t icao, const Frame& f, bool* created) {
    auto it = std::find_if(list_.begin(), list_.end(), [icao](const Aircraft& a) { return a.icao == icao; });
    if (it != list_.end()) { *created = false; return *it; }
    Aircraft a;
    a.icao = icao;
    a.first_seen_s = f.t_s;
    a.rssi_db = f.rssi_db;
    list_.push_back(a);
    *created = true;
    return list_.back();
}

bool AircraftTable::resolve_position(Aircraft& a, const Message& m, double t, Position* out) {
    auto& slot = m.cpr_odd ? a.cpr_odd : a.cpr_even;
    slot = {m.cpr_lat, m.cpr_lon, t};
    // 前回位置があれば局所解(速く、ゾーン境界に強い)。無ければ偶奇ペアの大域解、それも無ければ基準位置からの局所解
    if (a.position) return cpr_local(m.cpr_lat, m.cpr_lon, m.cpr_odd, *a.position, out);
    const auto& other = m.cpr_odd ? a.cpr_even : a.cpr_odd;
    if (std::fabs(t - other.t_s) <= cfg_.cpr_pair_s) {
        const bool ok = m.cpr_odd ? cpr_global(other.lat, other.lon, m.cpr_lat, m.cpr_lon, true, out)
                                  : cpr_global(m.cpr_lat, m.cpr_lon, other.lat, other.lon, false, out);
        if (ok) return true;
    }
    if (ref_) return cpr_local(m.cpr_lat, m.cpr_lon, m.cpr_odd, *ref_, out);
    return false;
}

const Aircraft& AircraftTable::update(const Frame& f) {
    const Message& m = f.msg;
    bool created = false;
    Aircraft& a = get_or_create(m.icao, f, &created);
    a.last_seen_s = f.t_s;
    ++a.messages;
    a.rssi_db = created ? f.rssi_db : 0.8 * a.rssi_db + 0.2 * f.rssi_db;
    if (m.has_callsign) { a.callsign = m.callsign; a.category = m.category; }
    if (m.has_squawk) a.squawk = m.squawk;
    if (m.has_altitude) { if (m.alt_source == AltSource::Gnss) a.altitude_gnss_ft = m.altitude_ft; else a.altitude_ft = m.altitude_ft; }
    if (m.has_velocity) { a.gs_kt = m.gs_kt; a.track_deg = m.track_deg; a.track_is_heading = m.track_is_heading; }
    if (m.has_vr) a.vr_fpm = m.vr_fpm;
    if (m.df == 17 || m.df == 18 || m.df == 0 || m.df == 4 || m.df == 5 || m.df == 16 || m.df == 20 || m.df == 21) {
        // 地上フラグは TC5–8 / VS / FS から。空中位置(TC9–18)が来たら降ろす
        if (m.on_ground) a.on_ground = true;
        else if (m.has_cpr) a.on_ground = false;
    }
    if (created && cb_.created) cb_.created(a, f);
    if (m.has_cpr) {
        Position p;
        if (resolve_position(a, m, f.t_s, &p)) {
            bool ok = true;
            if (ref_) {
                const double d = distance_km(*ref_, p);
                if (cfg_.max_range_km > 0 && d > cfg_.max_range_km) ok = false;
                else { a.distance_km = d; a.bearing_deg = bearing_deg(*ref_, p); }
            }
            if (ok) {
                const bool first = !a.position.has_value();
                a.position = p;
                a.last_position_s = f.t_s;
                ++a.positions;
                ++total_positions_;
                if (first && cb_.first_position) cb_.first_position(a, f);
            } else {
                ++rejected_positions_;
                if (!a.position) { a.cpr_even = {}; a.cpr_odd = {}; }   // 怪しいペアは捨てて取り直す
            }
        }
    }
    return a;
}

void AircraftTable::expire(double now_s) {
    for (auto it = list_.begin(); it != list_.end();) {
        if (now_s - it->last_seen_s > cfg_.expire_s) { if (cb_.lost) cb_.lost(*it); it = list_.erase(it); }
        else ++it;
    }
}

void AircraftTable::clear() { list_.clear(); }

} // namespace spear::adsb
