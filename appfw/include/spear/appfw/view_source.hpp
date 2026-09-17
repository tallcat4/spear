// spear-gui — ViewSource: ViewProcessor を QML の item から参照するための薄い QObject
#pragma once

#include "view_processor.hpp"

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace spear::appfw {

class ViewSource : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("created by the application")
    Q_PROPERTY(double dbMin READ dbMin WRITE setDbMin NOTIFY rangeChanged)
    Q_PROPERTY(double dbMax READ dbMax WRITE setDbMax NOTIFY rangeChanged)
    Q_PROPERTY(int averaging READ averaging WRITE setAveraging NOTIFY averagingChanged)
public:
    explicit ViewSource(QObject* parent = nullptr) : QObject(parent) {}
    void setProcessor(ViewProcessor* p) { vp_ = p; Q_EMIT processorChanged(); Q_EMIT rangeChanged(); }
    ViewProcessor* processor() const { return vp_; }

    double dbMin() const { float lo = -110, hi = -20; if (vp_) vp_->get_db_range(lo, hi); return lo; }
    double dbMax() const { float lo = -110, hi = -20; if (vp_) vp_->get_db_range(lo, hi); return hi; }
    void setDbMin(double v) { if (!vp_) return; float lo, hi; vp_->get_db_range(lo, hi); vp_->set_db_range(static_cast<float>(v), hi); Q_EMIT rangeChanged(); }
    void setDbMax(double v) { if (!vp_) return; float lo, hi; vp_->get_db_range(lo, hi); vp_->set_db_range(lo, static_cast<float>(v)); Q_EMIT rangeChanged(); }
    int averaging() const { return avg_; }
    void setAveraging(int n) { avg_ = std::max(1, std::min(n, 64)); if (vp_) vp_->set_averaging(avg_); Q_EMIT averagingChanged(); }
    Q_INVOKABLE void resetMaxHold() { if (vp_) vp_->reset_max_hold(); }
    Q_INVOKABLE void autoRange() { if (vp_) vp_->request_auto_range(); }
    Q_INVOKABLE void shiftRange(double db) { if (!vp_) return; float lo, hi; vp_->get_db_range(lo, hi); vp_->set_db_range(lo + static_cast<float>(db), hi + static_cast<float>(db)); Q_EMIT rangeChanged(); }
    Q_INVOKABLE void scaleRange(double factor) { if (!vp_) return; float lo, hi; vp_->get_db_range(lo, hi); const float mid = (lo + hi) / 2, half = (hi - lo) / 2 * static_cast<float>(factor); vp_->set_db_range(mid - half, mid + half); Q_EMIT rangeChanged(); }

Q_SIGNALS:
    void processorChanged();
    void rangeChanged();
    void averagingChanged();

private:
    ViewProcessor* vp_ = nullptr;
    int avg_ = 4;
};

} // namespace spear::appfw
