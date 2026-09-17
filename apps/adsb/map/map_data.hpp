// ミニマップの地図データ(純 C++、Qt 非依存)。
// apps/adsb/map/world.spearmap(build_map.py が Natural Earth 1:10m + OurAirports から作る。どちらもパブリックドメイン)を
// バイナリに埋め込み、最初の利用時に展開する。全世界の陸地 / 湖 / 国境 / 日本の都道府県境 / 都市 / 空港 / 滑走路。
// 座標は経度・緯度(度)。折れ線ごとに bbox を持ち、描画側は表示範囲と交わるものだけを描く。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spear::adsb::map {

struct LonLat { float lon = 0, lat = 0; };

struct Polyline {
    std::vector<LonLat> pts;
    float lon_min = 0, lon_max = 0, lat_min = 0, lat_max = 0;
    bool intersects(float lo0, float la0, float lo1, float la1) const { return !(lon_max < lo0 || lon_min > lo1 || lat_max < la0 || lat_min > la1); }
};

struct Place { LonLat p; uint32_t pop = 0; uint8_t rank = 10; std::string name; };
struct Airport { LonLat p; uint8_t kind = 2; std::string ident, name; };   // kind: 0 large, 1 medium, 2 small
struct Runway { LonLat a, b; float length_m = 0; };

struct MapData {
    std::vector<Polyline> land, lakes, borders, admin1;
    std::vector<Place> places;
    std::vector<Airport> airports;
    std::vector<Runway> runways;
};

// 埋め込みデータ(world.spearmap)を展開する。壊れていれば err に理由、戻り値 false
bool parse(std::span<const uint8_t> blob, MapData& out, std::string* err = nullptr);
std::span<const uint8_t> embedded_blob();
// 一度だけ展開した共有インスタンス(失敗時は空)
const MapData& world();

} // namespace spear::adsb::map
