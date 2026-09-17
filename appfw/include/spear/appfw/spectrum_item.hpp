// spear-gui — Spectrum trace (要件 §9.3 Spectrum widget)
// QSGGeometryNode の line strip。live trace + max hold。グリッド・目盛は QML 側(Rectangle/Text)。
#pragma once

#include "view_source.hpp"

#include <QColor>
#include <QQuickItem>

namespace spear::appfw {

class SpectrumItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(ViewSource* source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QColor traceColor MEMBER trace_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor maxHoldColor MEMBER max_color_ NOTIFY styleChanged)
    Q_PROPERTY(bool maxHold MEMBER max_hold_ NOTIFY styleChanged)
    Q_PROPERTY(double dbMin MEMBER db_min_ NOTIFY styleChanged)
    Q_PROPERTY(double dbMax MEMBER db_max_ NOTIFY styleChanged)
    // 読み出し(QML の表示用)
    Q_PROPERTY(double peakDb READ peakDb NOTIFY frameChanged)
    Q_PROPERTY(double peakOffsetHz READ peakOffsetHz NOTIFY frameChanged)
    Q_PROPERTY(double noiseFloorDb READ noiseFloorDb NOTIFY frameChanged)
    Q_PROPERTY(double sampleRate READ sampleRate NOTIFY frameChanged)
    Q_PROPERTY(double centerFreq READ centerFreq NOTIFY frameChanged)
    Q_PROPERTY(bool discontinuity READ discontinuity NOTIFY frameChanged)
    Q_PROPERTY(double fps READ fps NOTIFY frameChanged)
public:
    explicit SpectrumItem(QQuickItem* parent = nullptr);
    ViewSource* source() const { return source_; }
    void setSource(ViewSource* s);
    double peakDb() const { return frame_.peak_db; }
    double peakOffsetHz() const { return frame_.peak_offset_hz; }
    double noiseFloorDb() const { return frame_.noise_floor_db; }
    double sampleRate() const { return frame_.sample_rate; }
    double centerFreq() const { return frame_.center_freq; }
    bool discontinuity() const { return frame_.flags.has(spear::Flag::Discontinuity); }
    double fps() const { return fps_; }

Q_SIGNALS:
    void sourceChanged();
    void styleChanged();
    void frameChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void timerEvent(QTimerEvent*) override;

private:
    ViewSource* source_ = nullptr;
    SpectrumFrame frame_;
    uint64_t seen_ = 0;
    QColor trace_color_{0x3c, 0xf0, 0x3c};
    QColor max_color_{0x90, 0x70, 0x10};
    bool max_hold_ = true;
    double db_min_ = -110, db_max_ = -20;
    int timer_ = 0;
    float last_lo_ = 0, last_hi_ = 0;
    double fps_ = 0;
    uint64_t fps_frames_ = 0;
    qint64 fps_t0_ = 0;
};

} // namespace spear::appfw
