#include "base_layer.hpp"

#include <QFont>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace spear::map {

namespace {
// 表示 bbox(経度は [-180,180] からはみ出し得る)と交わるか。はみ出した分は 360° ずらした写しでも見る(日付変更線をまたぐ表示)
bool in_view(float lon_min, float lon_max, float lat_min, float lat_max, double lo0, double la0, double lo1, double la1) {
    if (lat_max < la0 || lat_min > la1) return false;
    for (double off : {0.0, 360.0, -360.0})
        if (!(lon_max < lo0 + off || lon_min > lo1 + off)) return true;
    return false;
}

void draw_lines(QPainter& p, const View& view, const std::vector<Polyline>& lines, double lo0, double la0, double lo1, double la1, int w, int h, bool close_fill) {
    QPainterPath path;
    QPolygonF poly;
    for (const auto& pl : lines) {
        if (!in_view(pl.lon_min, pl.lon_max, pl.lat_min, pl.lat_max, lo0, la0, lo1, la1)) continue;
        poly.clear();
        poly.reserve(static_cast<int>(pl.pts.size()));
        for (const auto& q : pl.pts) { double px, py; view.to_px(q.lon, q.lat, w, h, &px, &py); poly.append(QPointF(px, py)); }
        if (close_fill) path.addPolygon(poly);
        else p.drawPolyline(poly);
    }
    if (close_fill) p.drawPath(path);
}

double nice_step(double raw) {
    static const double steps[] = {0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10, 20, 30};
    for (double s : steps) if (s >= raw) return s;
    return 30;
}
double nice_km(double raw) {
    static const double steps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000};
    double best = 1;
    for (double s : steps) if (s <= raw) best = s;
    return best;
}
} // namespace

QImage render_base(const View& view, int w, int h, const Style& st) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0, 0, 0));
    const auto& world = map::world();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    double lo0, la0, lo1, la1;
    view.bbox(&lo0, &la0, &lo1, &la1);
    const double ml = (lo1 - lo0) * 0.05, mp = (la1 - la0) * 0.05;
    lo0 -= ml; lo1 += ml; la0 -= mp; la1 += mp;
    const double span = view.span_km;
    QFont small(st.font_family, 13);

    // 陸・湖
    p.setPen(QPen(st.coast, 1));
    p.setBrush(st.land);
    draw_lines(p, view, world.land, lo0, la0, lo1, la1, w, h, true);
    p.setBrush(QColor(0, 0, 0));
    draw_lines(p, view, world.lakes, lo0, la0, lo1, la1, w, h, true);
    // 境界
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(st.line, 1, Qt::DashLine));
    draw_lines(p, view, world.borders, lo0, la0, lo1, la1, w, h, false);
    if (span < 1500) draw_lines(p, view, world.admin1, lo0, la0, lo1, la1, w, h, false);
    // 経緯度線(4〜8 本になる間隔)
    {
        const double step_lat = nice_step((la1 - la0) / 5), step_lon = nice_step((lo1 - lo0) / 5);
        p.setPen(QPen(st.line, 1, Qt::DotLine));
        p.setFont(small);
        for (double la = std::ceil(la0 / step_lat) * step_lat; la <= la1; la += step_lat) {
            double px, py; view.to_px(view.center_lon, la, w, h, &px, &py);
            p.drawLine(QPointF(0, py), QPointF(w, py));
            p.setPen(st.dim); p.drawText(QPointF(4, py - 3), QString::number(la, 'f', step_lat < 1 ? 2 : 0) + "°"); p.setPen(QPen(st.line, 1, Qt::DotLine));
        }
        for (double lo = std::ceil(lo0 / step_lon) * step_lon; lo <= lo1; lo += step_lon) {
            double px, py; view.to_px(lo, view.center_lat, w, h, &px, &py);
            p.drawLine(QPointF(px, 0), QPointF(px, h));
            p.setPen(st.dim); p.drawText(QPointF(px + 3, h - 6), QString::number(View::wrap_lon(lo), 'f', step_lon < 1 ? 2 : 0) + "°"); p.setPen(QPen(st.line, 1, Qt::DotLine));
        }
    }
    // 滑走路(実寸の線)と空港
    if (st.airports && span < 250) {
        p.setPen(QPen(st.airport, 3));
        for (const auto& r : world.runways) {
            if (!in_view(r.a.lon, r.a.lon, r.a.lat, r.a.lat, lo0, la0, lo1, la1)) continue;
            double x1, y1, x2, y2;
            view.to_px(r.a.lon, r.a.lat, w, h, &x1, &y1);
            view.to_px(r.b.lon, r.b.lat, w, h, &x2, &y2);
            p.drawLine(QPointF(x1, y1), QPointF(x2, y2));
        }
    }
    if (st.airports && span < 1500) {
        p.setFont(small);
        for (const auto& a : world.airports) {
            if (!in_view(a.p.lon, a.p.lon, a.p.lat, a.p.lat, lo0, la0, lo1, la1)) continue;
            if (a.kind == 1 && span > 600) continue;
            if (a.kind == 2 && span > 200) continue;
            double px, py; view.to_px(a.p.lon, a.p.lat, w, h, &px, &py);
            p.setPen(QPen(st.airport, 1));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(px, py), 5, 5);
            p.drawLine(QPointF(px - 8, py), QPointF(px + 8, py));
            if (span < 600) p.drawText(QPointF(px + 9, py - 4), QString::fromStdString(a.ident));
        }
    }
    // 都市(表示幅に応じて人口で間引く)
    {
        const uint32_t min_pop = span > 800 ? 1'000'000 : span > 300 ? 300'000 : span > 120 ? 100'000 : 0;
        p.setFont(small);
        for (const auto& c : world.places) {
            if (c.pop < min_pop || !in_view(c.p.lon, c.p.lon, c.p.lat, c.p.lat, lo0, la0, lo1, la1)) continue;
            double px, py; view.to_px(c.p.lon, c.p.lat, w, h, &px, &py);
            p.setPen(Qt::NoPen); p.setBrush(st.dim);
            p.drawRect(QRectF(px - 2, py - 2, 4, 4));
            p.setPen(st.dim);
            p.drawText(QPointF(px + 6, py + 5), QString::fromStdString(c.name));
        }
    }
    return img;
}

void draw_scale_bar(QPainter& p, const View& view, int w, int h, const Style& st) {
    const double km = nice_km(view.span_km / 5);
    const double px_len = km / view.span_km * w;
    const double x0 = 12, y0 = h - 28;
    p.setPen(QPen(st.text, 2));
    p.drawLine(QPointF(x0, y0), QPointF(x0 + px_len, y0));
    p.drawLine(QPointF(x0, y0 - 5), QPointF(x0, y0 + 5));
    p.drawLine(QPointF(x0 + px_len, y0 - 5), QPointF(x0 + px_len, y0 + 5));
    p.setFont(QFont(st.font_family, 13));
    p.drawText(QPointF(x0 + px_len + 8, y0 + 5), QString::number(km) + " km");
}

bool Canvas::step_fit(const std::vector<Geo>& pts, double aspect) {
    if (!auto_fit) return false;
    const View target = fit_target(pts, aspect, fit, view);
    if (!ease(view, target, pts, fit)) return false;
    dirty_ = true;
    return true;
}

bool Canvas::set_auto_fit(bool on, const std::vector<Geo>& pts, double aspect) {
    if (on == auto_fit) return false;
    auto_fit = on;
    if (on) { fit.expand_alpha = 1.0; step_fit(pts, aspect); fit.expand_alpha = FitConfig{}.expand_alpha; }   // FIT は即座に合わせる
    dirty_ = true;
    return true;
}

void Canvas::zoom(double factor) {
    auto_fit = false;
    view.span_km = std::clamp(view.span_km * factor, 5.0, 5000.0);
    dirty_ = true;
}

void Canvas::pan(double dx_px, double dy_px, double width_px) {
    if (width_px <= 0) return;
    auto_fit = false;
    const double km_per_px = view.span_km / width_px;
    view.center_lon = View::wrap_lon(view.center_lon - dx_px * km_per_px / std::max(1e-6, view.km_per_deg_lon()));
    view.center_lat += dy_px * km_per_px / View::kKmPerDegLat;
    view.center_lat = std::clamp(view.center_lat, -85.0, 85.0);
    dirty_ = true;
}

const QImage& Canvas::base(int w, int h, const Style& style) {
    if (dirty_ || base_.size() != QSize(w, h)) { base_ = render_base(view, w, h, style); dirty_ = false; }
    return base_;
}

} // namespace spear::map
