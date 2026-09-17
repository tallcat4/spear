#include "spear/appfw/waterfall_item.hpp"

#include <QDateTime>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGTextureMaterial>
#include <rhi/qrhi.h>

#include <algorithm>

namespace spear::appfw {

// ---- RingTexture ---------------------------------------------------------------

RingTexture::RingTexture(int width, int rows) : w_(width), rows_(rows) {
    setFiltering(QSGTexture::Nearest);
    setHorizontalWrapMode(QSGTexture::ClampToEdge);
    setVerticalWrapMode(QSGTexture::Repeat); // リング: v が 1 を超えて折り返す
}

RingTexture::~RingTexture() { delete tex_; }

qint64 RingTexture::comparisonKey() const { return reinterpret_cast<qint64>(this); }

void RingTexture::enqueue_row(uint64_t row_index, const uint32_t* px) {
    pending_.push_back({row_index, std::vector<uint32_t>(px, px + w_)});
}

void RingTexture::commitTextureOperations(QRhi* rhi, QRhiResourceUpdateBatch* updates) {
    if (!tex_) {
        tex_ = rhi->newTexture(QRhiTexture::RGBA8, QSize(w_, rows_));
        if (!tex_->create()) { delete tex_; tex_ = nullptr; return; }
    }
    if (!cleared_) {
        // 初期化: 全面黒
        cleared_ = true;
        QByteArray zeros(static_cast<qsizetype>(w_) * rows_ * 4, '\0');
        // alpha = 0xff
        for (qsizetype i = 3; i < zeros.size(); i += 4) zeros[i] = static_cast<char>(0xff);
        QRhiTextureSubresourceUploadDescription sub(zeros.constData(), zeros.size());
        sub.setSourceSize(QSize(w_, rows_));
        updates->uploadTexture(tex_, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
    }
    // 行ごとの部分アップロード(通常 1 フレームあたり 1〜数行)
    for (auto& r : pending_) {
        QRhiTextureSubresourceUploadDescription sub(r.px.data(), static_cast<quint32>(r.px.size() * 4));
        sub.setSourceSize(QSize(w_, 1));
        sub.setDestinationTopLeft(QPoint(0, static_cast<int>(r.index % static_cast<uint64_t>(rows_))));
        updates->uploadTexture(tex_, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
    }
    pending_.clear();
}

// ---- WaterfallItem -------------------------------------------------------------

namespace {
struct WfNode : QSGGeometryNode {
    RingTexture* tex = nullptr;   // material が所有(QSGTextureMaterial は所有しないので自前で delete)
    QSGTextureMaterial* mat = nullptr;
    ~WfNode() override { delete tex; }
};
} // namespace

WaterfallItem::WaterfallItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    timer_ = startTimer(33);
}

void WaterfallItem::timerEvent(QTimerEvent*) {
    if (!source_ || !source_->processor()) return;
    if (source_->processor()->rows_written() != next_row_) update();
}

void WaterfallItem::releaseResources() {
    next_row_ = 0; // node が作り直されるので全行を再取得
}

QSGNode* WaterfallItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* node = static_cast<WfNode*>(old);
    ViewProcessor* vp = (source_ ? source_->processor() : nullptr);
    if (!vp || width() < 2 || height() < 2) { delete node; return nullptr; }
    const int W = static_cast<int>(vp->width());
    const int R = static_cast<int>(vp->rows());
    if (!node) {
        node = new WfNode;
        node->tex = new RingTexture(W, R);
        node->mat = new QSGTextureMaterial;
        node->mat->setTexture(node->tex);
        node->mat->setFiltering(QSGTexture::Nearest);
        node->mat->setVerticalWrapMode(QSGTexture::Repeat);
        node->mat->setHorizontalWrapMode(QSGTexture::ClampToEdge);
        node->setMaterial(node->mat);
        node->setFlag(QSGNode::OwnsMaterial);
        auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
        g->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(g);
        node->setFlag(QSGNode::OwnsGeometry);
        next_row_ = 0;
    }
    // 新しい行を取り込む(GUI thread は sync 中でブロックされているので vp の ring は安全に読める)
    const uint64_t written = vp->rows_written();
    if (written != next_row_) {
        const uint64_t first = vp->copy_rows(next_row_, tmp_);
        const uint64_t n = (written - first);
        for (uint64_t i = 0; i < n; ++i) node->tex->enqueue_row(first + i, tmp_.data() + i * static_cast<uint64_t>(W));
        rps_rows_ += n;
        next_row_ = written;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (rps_t0_ == 0) rps_t0_ = now;
        if (now - rps_t0_ >= 1000) { rps_ = static_cast<double>(rps_rows_) * 1000.0 / static_cast<double>(now - rps_t0_); rps_rows_ = 0; rps_t0_ = now; Q_EMIT statsChanged(); }
    }
    // 表示: 最新行を上端に。item の高さ分だけ行を見せる(1 行 = 1 px。高さが R を超える場合は引き伸ばし)
    const float w = static_cast<float>(width()), h = static_cast<float>(height());
    const int visible = std::min(static_cast<int>(h), R);
    rows_visible_ = visible;
    // 最新行 = written-1 → v_top = written/R(そのすぐ下が最新行)。下端 = (written - visible)/R
    const float v_top = static_cast<float>(written % static_cast<uint64_t>(R)) / static_cast<float>(R);
    const float v_bot = v_top - static_cast<float>(visible) / static_cast<float>(R);
    auto* v = node->geometry()->vertexDataAsTexturedPoint2D();
    v[0].set(0, 0, 0.f, v_top);
    v[1].set(w, 0, 1.f, v_top);
    v[2].set(0, h, 0.f, v_bot);
    v[3].set(w, h, 1.f, v_bot);
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    return node;
}

} // namespace spear::appfw
