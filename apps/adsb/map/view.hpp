// ミニマップの表示範囲(純 C++)。中心と横幅 [km] で持ち、局所等距円筒図法で画面に写す(数百 km の範囲では十分)。
// 自動フィット: 位置のある全機を含む bbox に余白をつけた範囲へ寄せる。広げる方向は速く、狭める方向はゆっくり(機が消えた
// 瞬間に画面が跳ねない)。手動操作(パン / ズーム)中は追従しない。
#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace spear::adsb::map {

struct Geo { double lon = 0, lat = 0; };

struct View {
    double center_lon = 139.77, center_lat = 35.68;   // 位置が無いときの初期表示(東京)
    double span_km = 300;                             // 画面の横幅
    double aspect = 1.0;                              // 幅 / 高さ

    static constexpr double kKmPerDegLat = 111.32;
    double km_per_deg_lon() const { return kKmPerDegLat * std::cos(center_lat * std::numbers::pi / 180); }
    double span_lat_km() const { return span_km / aspect; }
    // 経緯度 → 中心からの km(x 東、y 北)
    void to_km(double lon, double lat, double* x, double* y) const {
        *x = (lon - center_lon) * km_per_deg_lon();
        *y = (lat - center_lat) * kKmPerDegLat;
    }
    // 画面(幅 w、高さ h、y 下向き)への写像
    void to_px(double lon, double lat, double w, double h, double* px, double* py) const {
        double x, y; to_km(lon, lat, &x, &y);
        const double s = w / span_km;
        *px = w / 2 + x * s; *py = h / 2 - y * s;
    }
    void px_to_geo(double px, double py, double w, double h, double* lon, double* lat) const {
        const double s = w / span_km;
        *lon = center_lon + (px - w / 2) / s / km_per_deg_lon();
        *lat = center_lat - (py - h / 2) / s / kKmPerDegLat;
    }
    // 表示範囲の経緯度 bbox(描画の間引き用)
    void bbox(double* lo0, double* la0, double* lo1, double* la1) const {
        const double dl = span_km / 2 / std::max(1e-6, km_per_deg_lon()), dp = span_lat_km() / 2 / kKmPerDegLat;
        *lo0 = center_lon - dl; *lo1 = center_lon + dl; *la0 = center_lat - dp; *la1 = center_lat + dp;
    }
    bool contains(double lon, double lat, double margin_frac = 0) const {
        double x, y; to_km(lon, lat, &x, &y);
        return std::fabs(x) <= span_km / 2 * (1 - margin_frac) && std::fabs(y) <= span_lat_km() / 2 * (1 - margin_frac);
    }
};

struct FitConfig {
    double min_span_km = 20;      // 1 機だけでもこの幅は見せる
    double margin = 0.15;         // bbox の外側の余白(片側)
    double expand_alpha = 0.5;    // 広げるときの 1 tick あたりの追従率(機が画面外に出たら速く)
    double shrink_alpha = 0.04;   // 狭める・中心を寄せるとき(機が消えても画面が跳ねない)
};

// 全機を含む目標範囲
inline View fit_target(const std::vector<Geo>& pts, double aspect, const FitConfig& cfg, const View& fallback) {
    if (pts.empty()) { View v = fallback; v.aspect = aspect; return v; }
    double lo0 = 1e9, lo1 = -1e9, la0 = 1e9, la1 = -1e9;
    for (const auto& p : pts) { lo0 = std::min(lo0, p.lon); lo1 = std::max(lo1, p.lon); la0 = std::min(la0, p.lat); la1 = std::max(la1, p.lat); }
    View v;
    v.aspect = aspect;
    v.center_lon = (lo0 + lo1) / 2; v.center_lat = (la0 + la1) / 2;
    const double w = (lo1 - lo0) * v.km_per_deg_lon(), h = (la1 - la0) * View::kKmPerDegLat;
    v.span_km = std::max({cfg.min_span_km, w * (1 + 2 * cfg.margin), h * (1 + 2 * cfg.margin) * aspect});
    return v;
}

// 現在の表示を目標へ寄せる(1 tick)。戻り値 = 変化したか
inline bool ease(View& cur, const View& target, const std::vector<Geo>& pts, const FitConfig& cfg) {
    cur.aspect = target.aspect;
    bool outside = false;
    for (const auto& p : pts) if (!cur.contains(p.lon, p.lat, cfg.margin * 0.5)) { outside = true; break; }
    const double alpha = (outside || target.span_km > cur.span_km) ? cfg.expand_alpha : cfg.shrink_alpha;
    const double dx = target.center_lon - cur.center_lon, dy = target.center_lat - cur.center_lat, ds = target.span_km - cur.span_km;
    if (std::fabs(dx) < 1e-7 && std::fabs(dy) < 1e-7 && std::fabs(ds) < 1e-3) return false;
    cur.center_lon += dx * alpha;
    cur.center_lat += dy * alpha;
    cur.span_km += ds * alpha;
    return true;
}

} // namespace spear::adsb::map
