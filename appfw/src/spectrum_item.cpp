#include "spear/appfw/spectrum_item.hpp"

#include <QDateTime>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGOpacityNode>

namespace spear::appfw {

SpectrumItem::SpectrumItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    timer_ = startTimer(16); // GUI thread: 新 frame があれば update()
}

void SpectrumItem::setSource(ViewSource* s) {
    source_ = s;
    Q_EMIT sourceChanged();
}

void SpectrumItem::timerEvent(QTimerEvent*) {
    if (!source_ || !source_->processor()) return;
    if (source_->processor()->latest(frame_, seen_)) {
        seen_ = frame_.seq;
        float lo, hi;
        source_->processor()->get_db_range(lo, hi);
        if (lo != last_lo_ || hi != last_hi_) { last_lo_ = lo; last_hi_ = hi; Q_EMIT source_->rangeChanged(); }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (fps_t0_ == 0) fps_t0_ = now;
        ++fps_frames_;
        if (now - fps_t0_ >= 1000) { fps_ = static_cast<double>(fps_frames_) * 1000.0 / static_cast<double>(now - fps_t0_); fps_frames_ = 0; fps_t0_ = now; }
        Q_EMIT frameChanged();
        update();
    }
}

namespace {
struct Nodes : QSGNode {
    QSGOpacityNode* max_op = nullptr;
    QSGGeometryNode* max = nullptr;
    QSGGeometryNode* live = nullptr;
};
QSGGeometryNode* make_line(int n, const QColor& c) {
    auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), n);
    g->setDrawingMode(QSGGeometry::DrawLineStrip);
    g->setLineWidth(1.0f);
    auto* m = new QSGFlatColorMaterial;
    m->setColor(c);
    auto* node = new QSGGeometryNode;
    node->setGeometry(g);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(m);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}
void fill_line(QSGGeometryNode* node, const std::vector<float>& db, float w, float h, double lo, double hi, const QColor& c) {
    auto* g = node->geometry();
    const int n = static_cast<int>(db.size());
    if (g->vertexCount() != n) g->allocate(n);
    auto* v = g->vertexDataAsPoint2D();
    const float span = static_cast<float>(hi - lo);
    for (int i = 0; i < n; ++i) {
        const float x = n > 1 ? w * static_cast<float>(i) / static_cast<float>(n - 1) : 0.f;
        float t = (db[static_cast<std::size_t>(i)] - static_cast<float>(lo)) / span;
        if (t < -0.02f) t = -0.02f; if (t > 1.02f) t = 1.02f;
        v[i].set(x, h * (1.f - t));
    }
    static_cast<QSGFlatColorMaterial*>(node->material())->setColor(c);
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
}
} // namespace

QSGNode* SpectrumItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* root = static_cast<Nodes*>(old);
    const int n = static_cast<int>(frame_.db.size());
    if (n < 2) { delete root; return nullptr; }
    if (!root) {
        root = new Nodes;
        root->max = make_line(n, max_color_);
        root->live = make_line(n, trace_color_);
        root->max_op = new QSGOpacityNode;
        root->max_op->appendChildNode(root->max);
        root->appendChildNode(root->max_op);
        root->appendChildNode(root->live);
    }
    const float w = static_cast<float>(width()), h = static_cast<float>(height());
    if (max_hold_) {
        fill_line(root->max, frame_.max_hold, w, h, db_min_, db_max_, max_color_);
        root->max_op->setOpacity(1.0);
    } else {
        root->max_op->setOpacity(0.0);
    }
    fill_line(root->live, frame_.db, w, h, db_min_, db_max_, trace_color_);
    return root;
}

} // namespace spear::appfw
