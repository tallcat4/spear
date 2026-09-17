#include "map/map_item.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QVariantMap>

#include <cmath>
#include <unordered_set>

namespace spear::adsb {

using map::Geo;

MapItem::MapItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
}

void MapItem::setAircraft(const QVariantList& a) {
    aircraft_ = a;
    points_.clear();
    std::unordered_set<int> alive;
    for (const auto& v : aircraft_) {
        const auto m = v.toMap();
        const int icao = m["icao"].toInt();
        alive.insert(icao);
        if (!m["hasPos"].toBool()) continue;
        const Geo g{m["lon"].toDouble(), m["lat"].toDouble()};
        points_.push_back(g);
        auto& tr = trails_[icao];
        if (tr.empty() || std::fabs(tr.back().lon - g.lon) > 1e-5 || std::fabs(tr.back().lat - g.lat) > 1e-5) {
            tr.push_back(g);
            while (tr.size() > 400) tr.pop_front();
        }
    }
    for (auto it = trails_.begin(); it != trails_.end();) it = alive.count(it->first) ? std::next(it) : trails_.erase(it);
    step_fit();
    Q_EMIT aircraftChanged();
    update();
}

void MapItem::setAutoFit(bool on) {
    if (on == auto_fit_) return;
    auto_fit_ = on;
    if (on) { fit_.expand_alpha = 1.0; step_fit(); fit_.expand_alpha = map::FitConfig{}.expand_alpha; }   // FIT は即座に合わせる
    Q_EMIT viewChanged();
    update();
}

void MapItem::step_fit() {
    if (!auto_fit_) return;
    const double aspect = height() > 0 ? width() / height() : 1.0;
    target_ = map::fit_target(points_, aspect, fit_, view_);
    if (map::ease(view_, target_, points_, fit_)) { base_dirty_ = true; Q_EMIT viewChanged(); }
}

void MapItem::zoom(double factor) {
    auto_fit_ = false;
    view_.span_km = std::clamp(view_.span_km * factor, 5.0, 5000.0);
    base_dirty_ = true;
    Q_EMIT viewChanged();
    update();
}

void MapItem::pan(double dx_px, double dy_px) {
    if (width() <= 0) return;
    auto_fit_ = false;
    const double km_per_px = view_.span_km / width();
    view_.center_lon -= dx_px * km_per_px / std::max(1e-6, view_.km_per_deg_lon());
    view_.center_lat += dy_px * km_per_px / map::View::kKmPerDegLat;
    view_.center_lat = std::clamp(view_.center_lat, -85.0, 85.0);
    base_dirty_ = true;
    Q_EMIT viewChanged();
    update();
}

int MapItem::icaoAt(double x, double y) const {
    int best = 0;
    double bestd = 24.0 * 24.0;
    for (const auto& v : aircraft_) {
        const auto m = v.toMap();
        if (!m["hasPos"].toBool()) continue;
        double px, py;
        view_.to_px(m["lon"].toDouble(), m["lat"].toDouble(), width(), height(), &px, &py);
        const double d = (px - x) * (px - x) + (py - y) * (py - y);
        if (d < bestd) { bestd = d; best = m["icao"].toInt(); }
    }
    return best;
}

void MapItem::geometryChange(const QRectF& n, const QRectF& o) {
    QQuickPaintedItem::geometryChange(n, o);
    if (n.size() != o.size()) { view_.aspect = n.height() > 0 ? n.width() / n.height() : 1.0; base_dirty_ = true; step_fit(); }
}

void MapItem::draw_lines(QPainter& p, const std::vector<map::Polyline>& lines, double lo0, double la0, double lo1, double la1, int w, int h, bool close_fill) {
    QPainterPath path;
    QPolygonF poly;
    for (const auto& pl : lines) {
        if (!pl.intersects(static_cast<float>(lo0), static_cast<float>(la0), static_cast<float>(lo1), static_cast<float>(la1))) continue;
        poly.clear();
        poly.reserve(static_cast<int>(pl.pts.size()));
        for (const auto& q : pl.pts) { double px, py; view_.to_px(q.lon, q.lat, w, h, &px, &py); poly.append(QPointF(px, py)); }
        if (close_fill) path.addPolygon(poly);
        else p.drawPolyline(poly);
    }
    if (close_fill) p.drawPath(path);
}

namespace {
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
}

void MapItem::render_base(int w, int h) {
    base_ = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    base_.fill(QColor(0, 0, 0));
    base_view_ = view_;
    base_dirty_ = false;
    const auto& world = map::world();
    QPainter p(&base_);
    p.setRenderHint(QPainter::Antialiasing, true);
    double lo0, la0, lo1, la1;
    view_.bbox(&lo0, &la0, &lo1, &la1);
    const double ml = (lo1 - lo0) * 0.05, mp = (la1 - la0) * 0.05;
    lo0 -= ml; lo1 += ml; la0 -= mp; la1 += mp;
    const double span = view_.span_km;
    QFont small(font_family_, 13), base(font_family_, 15);

    // 陸・湖
    p.setPen(QPen(coast_color_, 1));
    p.setBrush(land_color_);
    draw_lines(p, world.land, lo0, la0, lo1, la1, w, h, true);
    p.setBrush(QColor(0, 0, 0));
    draw_lines(p, world.lakes, lo0, la0, lo1, la1, w, h, true);
    // 境界
    p.setBrush(Qt::NoBrush);
    QPen dashed(line_color_, 1, Qt::DashLine);
    p.setPen(dashed);
    draw_lines(p, world.borders, lo0, la0, lo1, la1, w, h, false);
    if (span < 1500) draw_lines(p, world.admin1, lo0, la0, lo1, la1, w, h, false);
    // 経緯度線(4〜8 本になる間隔)
    {
        const double step_lat = nice_step((la1 - la0) / 5), step_lon = nice_step((lo1 - lo0) / 5);
        p.setPen(QPen(line_color_, 1, Qt::DotLine));
        p.setFont(small);
        for (double la = std::ceil(la0 / step_lat) * step_lat; la <= la1; la += step_lat) {
            double px, py; view_.to_px(view_.center_lon, la, w, h, &px, &py);
            p.drawLine(QPointF(0, py), QPointF(w, py));
            p.setPen(dim_color_); p.drawText(QPointF(4, py - 3), QString::number(la, 'f', step_lat < 1 ? 2 : 0) + "°"); p.setPen(QPen(line_color_, 1, Qt::DotLine));
        }
        for (double lo = std::ceil(lo0 / step_lon) * step_lon; lo <= lo1; lo += step_lon) {
            double px, py; view_.to_px(lo, view_.center_lat, w, h, &px, &py);
            p.drawLine(QPointF(px, 0), QPointF(px, h));
            p.setPen(dim_color_); p.drawText(QPointF(px + 3, h - 6), QString::number(lo, 'f', step_lon < 1 ? 2 : 0) + "°"); p.setPen(QPen(line_color_, 1, Qt::DotLine));
        }
    }
    // 滑走路(実寸の線)と空港
    if (span < 250) {
        p.setPen(QPen(airport_color_, 3));
        for (const auto& r : world.runways) {
            if (r.a.lon < lo0 || r.a.lon > lo1 || r.a.lat < la0 || r.a.lat > la1) continue;
            double x1, y1, x2, y2;
            view_.to_px(r.a.lon, r.a.lat, w, h, &x1, &y1);
            view_.to_px(r.b.lon, r.b.lat, w, h, &x2, &y2);
            p.drawLine(QPointF(x1, y1), QPointF(x2, y2));
        }
    }
    if (span < 1500) {
        p.setFont(small);
        for (const auto& a : world.airports) {
            if (a.p.lon < lo0 || a.p.lon > lo1 || a.p.lat < la0 || a.p.lat > la1) continue;
            if (a.kind == 1 && span > 600) continue;
            if (a.kind == 2 && span > 200) continue;
            double px, py; view_.to_px(a.p.lon, a.p.lat, w, h, &px, &py);
            p.setPen(QPen(airport_color_, 1));
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
            if (c.p.lon < lo0 || c.p.lon > lo1 || c.p.lat < la0 || c.p.lat > la1 || c.pop < min_pop) continue;
            double px, py; view_.to_px(c.p.lon, c.p.lat, w, h, &px, &py);
            p.setPen(Qt::NoPen); p.setBrush(dim_color_);
            p.drawRect(QRectF(px - 2, py - 2, 4, 4));
            p.setPen(dim_color_);
            p.drawText(QPointF(px + 6, py + 5), QString::fromStdString(c.name));
        }
    }
}

void MapItem::paint(QPainter* p) {
    const int w = static_cast<int>(width()), h = static_cast<int>(height());
    if (w <= 0 || h <= 0) return;
    if (base_dirty_ || base_.size() != QSize(w, h)) render_base(w, h);
    p->drawImage(0, 0, base_);
    p->setRenderHint(QPainter::Antialiasing, true);
    QFont small(font_family_, 13), base(font_family_, 15);
    base.setBold(true);
    // 航跡
    for (const auto& [icao, tr] : trails_) {
        if (tr.size() < 2) continue;
        QPolygonF poly;
        for (const auto& g : tr) { double px, py; view_.to_px(g.lon, g.lat, w, h, &px, &py); poly.append(QPointF(px, py)); }
        QColor c = icao == selected_ ? selected_color_ : aircraft_color_;
        c.setAlpha(110);
        p->setPen(QPen(c, 1));
        p->drawPolyline(poly);
    }
    // 機体
    for (const auto& v : aircraft_) {
        const auto m = v.toMap();
        if (!m["hasPos"].toBool()) continue;
        double px, py;
        view_.to_px(m["lon"].toDouble(), m["lat"].toDouble(), w, h, &px, &py);
        if (px < -40 || py < -40 || px > w + 40 || py > h + 40) continue;
        const bool sel = m["icao"].toInt() == selected_;
        const double age = m["age"].toDouble();
        QColor c = sel ? selected_color_ : aircraft_color_;
        if (age > 30) c = dim_color_;
        p->setPen(QPen(c, 1));
        p->setBrush(c);
        const double trk = m["trk"].toDouble();
        if (trk >= 0) {
            p->save();
            p->translate(px, py);
            p->rotate(trk);
            const QPointF tri[3] = {{0, -9}, {6, 7}, {-6, 7}};
            p->drawPolygon(tri, 3);
            p->restore();
        } else {
            p->drawRect(QRectF(px - 5, py - 5, 10, 10));
        }
        if (sel) { p->setBrush(Qt::NoBrush); p->setPen(QPen(selected_color_, 2)); p->drawEllipse(QPointF(px, py), 16, 16); }
        QString name = m["callsign"].toString();
        if (name.isEmpty()) name = m["hex"].toString();
        const int alt = m["alt"].toInt();
        p->setFont(base);
        p->setPen(c);
        p->drawText(QPointF(px + 12, py - 2), name);
        p->setFont(small);
        p->setPen(sel ? selected_color_ : text_color_);
        p->drawText(QPointF(px + 12, py + 13), m["ground"].toBool() ? QString("GND") : (alt >= 0 ? QString::number(alt) + " ft" : QString("--")));
    }
    // スケールバー(左下)
    {
        const double km = nice_km(view_.span_km / 5);
        const double px_len = km / view_.span_km * w;
        const double x0 = 12, y0 = h - 28;
        p->setPen(QPen(text_color_, 2));
        p->drawLine(QPointF(x0, y0), QPointF(x0 + px_len, y0));
        p->drawLine(QPointF(x0, y0 - 5), QPointF(x0, y0 + 5));
        p->drawLine(QPointF(x0 + px_len, y0 - 5), QPointF(x0 + px_len, y0 + 5));
        p->setFont(small);
        p->drawText(QPointF(x0 + px_len + 8, y0 + 5), QString::number(km) + " km");
    }
}

} // namespace spear::adsb
