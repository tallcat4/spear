// spear-gui — ViewSource: ViewProcessor を QML の item から参照するための薄い QObject
// dB レンジは「自動(最初のフレームでノイズ床から決める)」か「手動(REF キー / ジェスチャ)」。手動の値は ViewSource が覚えていて、
// App を止めて次に processor が付いたときも同じレンジにする(SettingsStore で再起動をまたいで残せる: dbMin / dbMax / manualRange / averaging)。
#pragma once

#include "view_processor.hpp"

#include <QObject>
#include <QtQml/qqmlregistration.h>

#include <algorithm>
#include <utility>

namespace spear::appfw {

class ViewSource : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("created by the application")
    Q_PROPERTY(double dbMin READ dbMin WRITE setDbMin NOTIFY rangeChanged)
    Q_PROPERTY(double dbMax READ dbMax WRITE setDbMax NOTIFY rangeChanged)
    Q_PROPERTY(bool manualRange READ manualRange WRITE setManualRange NOTIFY rangeChanged)   // false = 次の processor で自動レンジ
    Q_PROPERTY(int averaging READ averaging WRITE setAveraging NOTIFY averagingChanged)
public:
    explicit ViewSource(QObject* parent = nullptr) : QObject(parent) {}
    void setProcessor(ViewProcessor* p) {
        vp_ = p;
        if (vp_) { vp_->set_averaging(avg_); if (manual_) { vp_->set_db_range(lo_, hi_); vp_->cancel_auto_range(); } }
        Q_EMIT processorChanged();
        Q_EMIT rangeChanged();
    }
    ViewProcessor* processor() const { return vp_; }

    double dbMin() const { return current().first; }
    double dbMax() const { return current().second; }
    void setDbMin(double v) { apply(static_cast<float>(v), current().second); }
    void setDbMax(double v) { apply(current().first, static_cast<float>(v)); }
    bool manualRange() const { return manual_; }
    // 手動/自動の決定はこのプロパティが持つ(復元の宣言順に依らない)。dbMin/dbMax の setter は processor が無い間は値を覚えるだけ
    void setManualRange(bool on) {
        manual_ = on;
        if (vp_) { if (on) { vp_->set_db_range(lo_, hi_); vp_->cancel_auto_range(); } else vp_->request_auto_range(); }
        Q_EMIT rangeChanged();
    }
    int averaging() const { return avg_; }
    void setAveraging(int n) { avg_ = std::max(1, std::min(n, 64)); if (vp_) vp_->set_averaging(avg_); Q_EMIT averagingChanged(); }
    Q_INVOKABLE void resetMaxHold() { if (vp_) vp_->reset_max_hold(); }
    Q_INVOKABLE void autoRange() { setManualRange(false); }
    Q_INVOKABLE void shiftRange(double db) { const auto [lo, hi] = current(); apply(lo + static_cast<float>(db), hi + static_cast<float>(db)); }
    Q_INVOKABLE void scaleRange(double factor) { const auto [lo, hi] = current(); const float mid = (lo + hi) / 2, half = (hi - lo) / 2 * static_cast<float>(factor); apply(mid - half, mid + half); }

Q_SIGNALS:
    void processorChanged();
    void rangeChanged();
    void averagingChanged();

private:
    std::pair<float, float> current() const { float lo = lo_, hi = hi_; if (vp_) vp_->get_db_range(lo, hi); return {lo, hi}; }
    void apply(float lo, float hi) {   // 表示中の操作(REF キー / ジェスチャ)= 手動に切り替えて今すぐ効かせる。停止中は値を覚えるだけ
        if (hi - lo < 10.f) hi = lo + 10.f;
        lo_ = lo; hi_ = hi;
        if (vp_) { manual_ = true; vp_->set_db_range(lo, hi); vp_->cancel_auto_range(); }
        Q_EMIT rangeChanged();
    }
    ViewProcessor* vp_ = nullptr;
    int avg_ = 4;
    bool manual_ = false;
    float lo_ = -110, hi_ = -20;
};

} // namespace spear::appfw
