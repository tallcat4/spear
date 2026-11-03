#include "position_map.hpp"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QVariantMap>

namespace spear::std_t99 {

PositionMap::PositionMap(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    canvas_.fit.min_span_km = 60;   // 1 か所だけでもこの幅は見せる(1:10m の地図で海岸線と周辺の都市が入り、位置関係がわかる幅)
    connect(this, &PositionMap::styleChanged, this, [this] { canvas_.invalidate(); update(); });
}

void PositionMap::setPins(const QVariantList& p) {
    pins_ = p;
    points_.clear();
    for (const auto& v : pins_) {
        const auto m = v.toMap();
        points_.push_back({m["lon"].toDouble(), m["lat"].toDouble()});
    }
    step_fit();
    Q_EMIT pinsChanged();
    update();
}

void PositionMap::step_fit() {
    // ピンが無いうちは初期表示(map::View の既定)のまま
    if (points_.empty()) return;
    // 自動フィットは 1 回の更新で目標まで寄せる(ピンは数秒に 1 回しか動かない。ADS-B のような毎 tick の追従は要らない)
    const double aspect = height() > 0 ? width() / height() : 1.0;
    const double alpha = canvas_.fit.expand_alpha, shrink = canvas_.fit.shrink_alpha;
    canvas_.fit.expand_alpha = canvas_.fit.shrink_alpha = 1.0;
    if (canvas_.step_fit(points_, aspect)) Q_EMIT viewChanged();
    canvas_.fit.expand_alpha = alpha;
    canvas_.fit.shrink_alpha = shrink;
}

void PositionMap::setAutoFit(bool on) {
    if (!canvas_.set_auto_fit(on, points_, height() > 0 ? width() / height() : 1.0)) return;
    step_fit();
    Q_EMIT viewChanged();
    update();
}

void PositionMap::zoom(double factor) {
    canvas_.zoom(factor);
    Q_EMIT viewChanged();
    update();
}

void PositionMap::pan(double dx_px, double dy_px) {
    if (width() <= 0) return;
    canvas_.pan(dx_px, dy_px, width());
    Q_EMIT viewChanged();
    update();
}

void PositionMap::geometryChange(const QRectF& n, const QRectF& o) {
    QQuickPaintedItem::geometryChange(n, o);
    if (n.size() != o.size()) { canvas_.resize(n.width(), n.height()); step_fit(); }
}

void PositionMap::paint(QPainter* p) {
    const int w = static_cast<int>(width()), h = static_cast<int>(height());
    if (w <= 0 || h <= 0) return;
    const auto& view = canvas_.view;
    p->drawImage(0, 0, canvas_.base(w, h, style()));
    p->setRenderHint(QPainter::Antialiasing, true);
    QFont label(font_family_, 15);
    label.setBold(true);
    p->setFont(label);
    // ほかの発信者(橙の点)を先に、最後の送信(赤いピン)を上に描く
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& v : pins_) {
            const auto m = v.toMap();
            const bool current = m["current"].toBool();
            if (current != (pass == 1)) continue;
            double px, py;
            view.to_px(m["lon"].toDouble(), m["lat"].toDouble(), w, h, &px, &py);
            if (px < -40 || py < -40 || px > w + 40 || py > h + 40) continue;
            const QColor c = current ? pin_color_ : other_color_;
            if (current) {
                // ピン: 先端が位置。頭は円
                QPainterPath pin;
                pin.moveTo(px, py);
                pin.lineTo(px - 9, py - 20);
                pin.arcTo(QRectF(px - 12, py - 36, 24, 24), 210, -240);
                pin.closeSubpath();
                p->setPen(QPen(QColor(0, 0, 0), 1));
                p->setBrush(c);
                p->drawPath(pin);
                p->setBrush(QColor(0, 0, 0));
                p->drawEllipse(QPointF(px, py - 24), 4, 4);
            } else {
                p->setPen(QPen(QColor(0, 0, 0), 1));
                p->setBrush(c);
                p->drawEllipse(QPointF(px, py), 7, 7);
            }
            p->setPen(c);
            p->drawText(QPointF(px + 14, current ? py - 22 : py + 5), m["label"].toString());
        }
    }
    map::draw_scale_bar(*p, view, w, h, style());
}

} // namespace spear::std_t99
