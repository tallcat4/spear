#include "map/map_item.hpp"

#include <QPainter>
#include <QVariantMap>

#include <cmath>
#include <unordered_set>

namespace spear::adsb {

using map::Geo;

MapItem::MapItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    connect(this, &MapItem::styleChanged, this, [this] { canvas_.invalidate(); update(); });
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
    if (!canvas_.set_auto_fit(on, points_, height() > 0 ? width() / height() : 1.0)) return;
    Q_EMIT viewChanged();
    update();
}

void MapItem::step_fit() {
    if (canvas_.step_fit(points_, height() > 0 ? width() / height() : 1.0)) Q_EMIT viewChanged();
}

void MapItem::zoom(double factor) {
    canvas_.zoom(factor);
    Q_EMIT viewChanged();
    update();
}

void MapItem::pan(double dx_px, double dy_px) {
    if (width() <= 0) return;
    canvas_.pan(dx_px, dy_px, width());
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
        canvas_.view.to_px(m["lon"].toDouble(), m["lat"].toDouble(), width(), height(), &px, &py);
        const double d = (px - x) * (px - x) + (py - y) * (py - y);
        if (d < bestd) { bestd = d; best = m["icao"].toInt(); }
    }
    return best;
}

void MapItem::geometryChange(const QRectF& n, const QRectF& o) {
    QQuickPaintedItem::geometryChange(n, o);
    if (n.size() != o.size()) { canvas_.resize(n.width(), n.height()); step_fit(); }
}

void MapItem::paint(QPainter* p) {
    const int w = static_cast<int>(width()), h = static_cast<int>(height());
    if (w <= 0 || h <= 0) return;
    const auto& view = canvas_.view;
    p->drawImage(0, 0, canvas_.base(w, h, style()));
    p->setRenderHint(QPainter::Antialiasing, true);
    QFont small(font_family_, 13), base(font_family_, 15);
    base.setBold(true);
    // 航跡
    for (const auto& [icao, tr] : trails_) {
        if (tr.size() < 2) continue;
        QPolygonF poly;
        for (const auto& g : tr) { double px, py; view.to_px(g.lon, g.lat, w, h, &px, &py); poly.append(QPointF(px, py)); }
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
        view.to_px(m["lon"].toDouble(), m["lat"].toDouble(), w, h, &px, &py);
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
    map::draw_scale_bar(*p, view, w, h, style());
}

} // namespace spear::adsb
