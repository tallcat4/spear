#include "spear/appfw/eye_diagram_item.hpp"

#include <QDateTime>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGOpacityNode>

namespace spear::appfw {

EyeDiagramItem::EyeDiagramItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    timer_ = startTimer(33);   // 30 fps で十分(トレースは数百本/秒溜まる)
}

void EyeDiagramItem::timerEvent(QTimerEvent*) {
    if (!source_) return;
    if (source_->snapshot(flat_, len_, n_traces_, seen_)) {
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
    QSGOpacityNode* old_op = nullptr;
    QSGGeometryNode* old_lines = nullptr;   // DrawLines: 各トレースを独立した線分列に
    QSGGeometryNode* recent_lines = nullptr;
};
QSGGeometryNode* make_lines(const QColor& c) {
    auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
    g->setDrawingMode(QSGGeometry::DrawLines);
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
// traces [from, to) を線分列(2 頂点 / 区間)として詰める
void fill(QSGGeometryNode* node, const std::vector<float>& flat, std::size_t len, std::size_t from, std::size_t to,
          float w, float h, double lo, double hi, const QColor& c) {
    auto* g = node->geometry();
    const int segs = (to > from && len > 1) ? static_cast<int>((to - from) * (len - 1)) : 0;
    if (g->vertexCount() != segs * 2) g->allocate(segs * 2);
    auto* v = g->vertexDataAsPoint2D();
    const float span = static_cast<float>(hi - lo);
    int k = 0;
    auto y_of = [&](float val) { float t = (val - static_cast<float>(lo)) / span; if (t < -0.05f) t = -0.05f; if (t > 1.05f) t = 1.05f; return h * (1.f - t); };
    for (std::size_t tr = from; tr < to; ++tr) {
        const float* p = flat.data() + tr * len;
        for (std::size_t i = 0; i + 1 < len; ++i) {
            const float x0 = w * static_cast<float>(i) / static_cast<float>(len - 1), x1 = w * static_cast<float>(i + 1) / static_cast<float>(len - 1);
            v[k++].set(x0, y_of(p[i]));
            v[k++].set(x1, y_of(p[i + 1]));
        }
    }
    static_cast<QSGFlatColorMaterial*>(node->material())->setColor(c);
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
}
} // namespace

QSGNode* EyeDiagramItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* root = static_cast<Nodes*>(old);
    if (len_ < 2 || n_traces_ == 0) { delete root; return nullptr; }
    if (!root) {
        root = new Nodes;
        root->old_lines = make_lines(trace_color_);
        root->recent_lines = make_lines(recent_color_);
        root->old_op = new QSGOpacityNode;
        root->old_op->appendChildNode(root->old_lines);
        root->appendChildNode(root->old_op);
        root->appendChildNode(root->recent_lines);
    }
    const float w = static_cast<float>(width()), h = static_cast<float>(height());
    const std::size_t recent = std::min<std::size_t>(static_cast<std::size_t>(std::max(recent_, 0)), n_traces_);
    fill(root->old_lines, flat_, len_, 0, n_traces_ - recent, w, h, y_min_, y_max_, trace_color_);
    root->old_op->setOpacity(persistence_);
    fill(root->recent_lines, flat_, len_, n_traces_ - recent, n_traces_, w, h, y_min_, y_max_, recent_color_);
    return root;
}

} // namespace spear::appfw
