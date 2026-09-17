// 航空機表 — 受理フレームを ICAO ごとに集約する(純 C++)。
// CPR は偶奇ペア(10 s 以内)の大域解を最初に求め、以後は前回位置(無ければ基準位置)からの局所解。
// 基準位置(地上局)があれば距離・方位を出し、max_range_km を超える位置は捨てる(大域解の多義性・ビット誤りの保険)。
#pragma once

#include "protocol/modes.hpp"
#include "receiver.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace spear::adsb {

struct Aircraft {
    uint32_t icao = 0;
    std::string callsign;
    int category = 0;
    std::optional<int> squawk;
    std::optional<int> altitude_ft;          // 気圧高度
    std::optional<int> altitude_gnss_ft;
    std::optional<double> gs_kt, track_deg;  // 速度・針路(TC19 subtype 3/4 では対気速度・機首方位)
    bool track_is_heading = false;
    std::optional<int> vr_fpm;
    std::optional<Position> position;
    std::optional<double> distance_km, bearing_deg;   // 基準位置があるとき
    bool on_ground = false;
    uint64_t messages = 0;                   // 受理フレーム数(DF 問わず)
    uint64_t positions = 0;                  // 位置が求まった回数
    double first_seen_s = 0, last_seen_s = 0, last_position_s = 0;
    double rssi_db = -100;                   // 直近フレームの平滑値
    // CPR の直近偶奇
    struct Cpr { uint32_t lat = 0, lon = 0; double t_s = -1e9; };
    Cpr cpr_even, cpr_odd;
};

struct TableConfig {
    double expire_s = 60;          // 最終受信からこの時間で表から消す
    double cpr_pair_s = 10;        // 偶奇ペアの最大時間差
    double max_range_km = 0;       // 基準位置からの上限(0 = 無制限)
};

class AircraftTable {
public:
    explicit AircraftTable(TableConfig cfg = {}) : cfg_(cfg) {}
    void set_reference(std::optional<Position> ref) { ref_ = ref; }
    const std::optional<Position>& reference() const { return ref_; }
    const TableConfig& config() const { return cfg_; }

    // 事象(App が Event にする)。位置は「初回確定」だけ
    struct Callbacks {
        std::function<void(const Aircraft&, const Frame&)> created;
        std::function<void(const Aircraft&, const Frame&)> first_position;
        std::function<void(const Aircraft&)> lost;
    };
    void set_callbacks(Callbacks cb) { cb_ = std::move(cb); }

    // 受理フレームを反映する。戻り値 = 更新した航空機(表に無ければ作る)
    const Aircraft& update(const Frame& f);
    // last_seen から expire_s 経った航空機を消す(now = 受信機の時間軸)
    void expire(double now_s);
    void clear();

    std::size_t size() const { return list_.size(); }
    const std::vector<Aircraft>& all() const { return list_; }
    const Aircraft* find(uint32_t icao) const;
    uint64_t total_positions() const { return total_positions_; }
    uint64_t rejected_positions() const { return rejected_positions_; }

private:
    Aircraft& get_or_create(uint32_t icao, const Frame& f, bool* created);
    bool resolve_position(Aircraft& a, const Message& m, double t, Position* out);
    TableConfig cfg_;
    std::optional<Position> ref_;
    Callbacks cb_;
    std::vector<Aircraft> list_;
    uint64_t total_positions_ = 0, rejected_positions_ = 0;
};

} // namespace spear::adsb
