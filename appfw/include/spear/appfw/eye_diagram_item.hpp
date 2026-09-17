// spear-gui — EyeDiagramItem (要件 §9.3): TraceSource のトレースを重ね描きする。
// 古いトレースは薄く(persistence)、直近の数本は明るく。目盛・ラベルは QML 側(EyeDiagram.qml)。
#pragma once

#include "trace_source.hpp"

#include <QColor>
#include <QQuickItem>

namespace spear::appfw {

class EyeDiagramItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(TraceSource* source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(double yMin MEMBER y_min_ NOTIFY styleChanged)
    Q_PROPERTY(double yMax MEMBER y_max_ NOTIFY styleChanged)
    Q_PROPERTY(QColor traceColor MEMBER trace_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor recentColor MEMBER recent_color_ NOTIFY styleChanged)
    Q_PROPERTY(int recentTraces MEMBER recent_ NOTIFY styleChanged)      // 明るく描く直近の本数
    Q_PROPERTY(double persistence MEMBER persistence_ NOTIFY styleChanged) // 古いトレースの不透明度
    Q_PROPERTY(int traces READ traces NOTIFY frameChanged)
    Q_PROPERTY(double fps READ fps NOTIFY frameChanged)
public:
    explicit EyeDiagramItem(QQuickItem* parent = nullptr);
    TraceSource* source() const { return source_; }
    void setSource(TraceSource* s) { source_ = s; seen_ = 0; Q_EMIT sourceChanged(); update(); }
    int traces() const { return static_cast<int>(n_traces_); }
    double fps() const { return fps_; }

Q_SIGNALS:
    void sourceChanged();
    void styleChanged();
    void frameChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void timerEvent(QTimerEvent*) override;

private:
    TraceSource* source_ = nullptr;
    std::vector<float> flat_;
    std::size_t len_ = 0, n_traces_ = 0;
    uint64_t seen_ = 0;
    double y_min_ = -4, y_max_ = 4;
    QColor trace_color_{0x3c, 0xf0, 0x3c};
    QColor recent_color_{0xe0, 0xff, 0xe0};
    int recent_ = 8;
    double persistence_ = 0.25;
    int timer_ = 0;
    double fps_ = 0;
    uint64_t fps_frames_ = 0;
    qint64 fps_t0_ = 0;
};

} // namespace spear::appfw
