// ミニマップ: 埋め込み地図データの展開と、表示範囲の自動フィット(Qt 非依存の部分)
#include "map/map_data.hpp"
#include "map/view.hpp"

#include <gtest/gtest.h>

using namespace spear::adsb::map;

namespace {
// 陸リング全体に対する even-odd の内外判定
bool on_land(const MapData& m, double lon, double lat) {
    bool inside = false;
    for (const auto& pl : m.land) {
        if (!pl.intersects(static_cast<float>(lon), static_cast<float>(lat), static_cast<float>(lon), static_cast<float>(lat))) continue;
        const auto& p = pl.pts;
        for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
            const bool cross = (p[i].lat > lat) != (p[j].lat > lat);
            if (cross && lon < (p[j].lon - p[i].lon) * (lat - p[i].lat) / (p[j].lat - p[i].lat) + p[i].lon) inside = !inside;
        }
    }
    return inside;
}
}

TEST(AdsbMap, EmbeddedDataParses) {
    const auto& w = world();
    EXPECT_GT(w.land.size(), 1000u);
    EXPECT_GT(w.lakes.size(), 100u);
    EXPECT_GT(w.borders.size(), 100u);
    EXPECT_GT(w.admin1.size(), 40u) << "日本の都道府県境";
    EXPECT_GT(w.places.size(), 500u);
    EXPECT_GT(w.airports.size(), 1000u);
    EXPECT_GT(w.runways.size(), 1000u);
    // 東京駅は陸、東京湾は海、太平洋は海
    EXPECT_TRUE(on_land(w, 139.767, 35.681));
    EXPECT_FALSE(on_land(w, 139.85, 35.45));
    EXPECT_FALSE(on_land(w, 150.0, 30.0));
    // 羽田(RJTT)がある。滑走路は 4 本
    int rjtt = 0;
    for (const auto& a : w.airports) if (a.ident == "RJTT") { ++rjtt; EXPECT_NEAR(a.p.lat, 35.55, 0.05); EXPECT_NEAR(a.p.lon, 139.78, 0.05); EXPECT_EQ(a.kind, 0); }
    EXPECT_EQ(rjtt, 1);
    bool tokyo = false;
    for (const auto& c : w.places) if (c.name == "Tokyo") tokyo = true;
    EXPECT_TRUE(tokyo);
}

TEST(AdsbMap, ParseRejectsGarbage) {
    MapData m;
    std::string err;
    const uint8_t junk[] = "SPMAP1\0\0\xff\xff\xff\xff";
    EXPECT_FALSE(parse(std::span<const uint8_t>(junk, sizeof junk - 1), m, &err));
    EXPECT_FALSE(parse(std::span<const uint8_t>(junk, 3), m, &err));
}

TEST(AdsbMap, FitCoversAllPointsAndEasesSmoothly) {
    FitConfig cfg;
    View v;   // 東京 300 km から始める
    v.aspect = 1.5;
    const std::vector<Geo> pts = {{139.80, 35.50}, {140.30, 36.20}, {139.40, 35.40}};
    const View t = fit_target(pts, 1.5, cfg, v);
    for (const auto& p : pts) EXPECT_TRUE(t.contains(p.lon, p.lat)) << "目標範囲は全機を含む";
    EXPECT_NEAR(t.span_km, 0.8 * View::kKmPerDegLat * 1.3 * 1.5, 0.5);   // 南北 0.8° ≈ 89 km × 余白 × アスペクトが支配的
    // 広げる方向は速く、狭める方向はゆっくり
    View cur = v;
    int n = 0;
    while (ease(cur, t, pts, cfg) && n < 500) ++n;
    EXPECT_LT(n, 500);
    EXPECT_NEAR(cur.span_km, t.span_km, 0.01);
    EXPECT_NEAR(cur.center_lat, t.center_lat, 1e-4);
    // 1 機だけ → 最小幅
    const View one = fit_target({{139.80, 35.50}}, 1.5, cfg, v);
    EXPECT_DOUBLE_EQ(one.span_km, cfg.min_span_km);
    EXPECT_NEAR(one.center_lon, 139.80, 1e-9);
    // 位置なし → fallback を保つ
    const View none = fit_target({}, 1.5, cfg, cur);
    EXPECT_DOUBLE_EQ(none.span_km, cur.span_km);
    // 日付変更線をまたぐ: 中心 179.9° から東へ 0.2° は −179.9° で、画面上は右隣
    View dl; dl.center_lon = 179.9; dl.center_lat = 0; dl.span_km = 100; dl.aspect = 1;
    double x, y;
    dl.to_km(-179.9, 0, &x, &y);
    EXPECT_NEAR(x, 0.2 * View::kKmPerDegLat, 0.01);
    EXPECT_TRUE(dl.contains(-179.95, 0));
    EXPECT_DOUBLE_EQ(View::wrap_lon(181), -179);
    EXPECT_DOUBLE_EQ(View::wrap_lon(-181), 179);
    EXPECT_DOUBLE_EQ(View::wrap_lon(180), -180);
    // 画面写像の往復
    double px, py, lon, lat;
    cur.to_px(139.9, 35.6, 800, 600, &px, &py);
    cur.px_to_geo(px, py, 800, 600, &lon, &lat);
    EXPECT_NEAR(lon, 139.9, 1e-9);
    EXPECT_NEAR(lat, 35.6, 1e-9);
}
