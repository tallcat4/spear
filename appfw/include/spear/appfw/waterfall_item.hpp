// spear-gui — Waterfall (要件 §9.3, §9.4)
//
// 自前の QSGTexture(QRhiTexture)を持つ scene graph node。view processor が生成した行を
// render thread で行単位に部分アップロードし、リング状のテクスチャ座標でスクロールする。
// 標準の QML Image 更新(毎フレーム全面再アップロード)はタブレット級 GPU で破綻する (§9.4)。
#pragma once

#include "view_source.hpp"

#include <QQuickItem>
#include <QSGTexture>

#include <memory>
#include <mutex>
#include <vector>

class QRhiTexture;

namespace spear::appfw {

// リング状テクスチャ。commitTextureOperations() で pending 行を QRhiResourceUpdateBatch に積む。
class RingTexture final : public QSGTexture {
public:
    RingTexture(int width, int rows);
    ~RingTexture() override;
    // GUI/render thread (sync 時) から: 行 (RGBA8, width px) を追加。row_index はリング上の行番号
    void enqueue_row(uint64_t row_index, const uint32_t* px);

    qint64 comparisonKey() const override;
    QRhiTexture* rhiTexture() const override { return tex_; }
    QSize textureSize() const override { return {w_, rows_}; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }
    void commitTextureOperations(QRhi* rhi, QRhiResourceUpdateBatch* updates) override;
    int rows() const { return rows_; }

private:
    int w_, rows_;
    QRhiTexture* tex_ = nullptr;
    struct Row { uint64_t index; std::vector<uint32_t> px; };
    std::vector<Row> pending_;
    bool cleared_ = false;
};

class WaterfallItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(ViewSource* source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(double rowsPerSecond READ rowsPerSecond NOTIFY statsChanged)
    Q_PROPERTY(int rowsVisible READ rowsVisible NOTIFY statsChanged)
public:
    explicit WaterfallItem(QQuickItem* parent = nullptr);
    ViewSource* source() const { return source_; }
    void setSource(ViewSource* s) { source_ = s; Q_EMIT sourceChanged(); }
    double rowsPerSecond() const { return rps_; }
    int rowsVisible() const { return rows_visible_; }

Q_SIGNALS:
    void sourceChanged();
    void statsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void timerEvent(QTimerEvent*) override;
    void releaseResources() override;

private:
    ViewSource* source_ = nullptr;
    uint64_t next_row_ = 0;      // まだ取り込んでいない最初の行
    std::vector<uint32_t> tmp_;
    int timer_ = 0;
    double rps_ = 0;
    int rows_visible_ = 0;
    uint64_t rps_rows_ = 0;
    qint64 rps_t0_ = 0;
};

} // namespace spear::appfw
