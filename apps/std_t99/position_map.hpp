// 発信位置の地図(QQuickPaintedItem)。埋め込み地図(widgets/map、ADS-B と共有)の上に、最後の送信の位置を赤いピン、
// 受信履歴にあるほかの発信者の最後の位置を橙の点で描く(std-t99-receiver-sharp の MapView と同じ)。
// 測位無効(A/V = V)と日本の範囲の外の座標は App が除いて渡す。表示範囲は自動フィット(ピンをすべて含む)。
// パン / ズームで手動になり、FIT で戻す。北が上。
#pragma once

#include "map/base_layer.hpp"

#include <QQuickPaintedItem>
#include <QVariantList>

namespace spear::std_t99 {

class PositionMap : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    // pins: [{lat, lon, label, current}]。current = 最後の送信(赤いピン)
    Q_PROPERTY(QVariantList pins READ pins WRITE setPins NOTIFY pinsChanged)
    Q_PROPERTY(bool autoFit READ autoFit WRITE setAutoFit NOTIFY viewChanged)
    Q_PROPERTY(double spanKm READ spanKm NOTIFY viewChanged)
    Q_PROPERTY(int pinCount READ pinCount NOTIFY pinsChanged)
    Q_PROPERTY(QColor landColor MEMBER land_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor coastColor MEMBER coast_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor lineColor MEMBER line_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER text_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor dimColor MEMBER dim_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor pinColor MEMBER pin_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor otherColor MEMBER other_color_ NOTIFY styleChanged)
    Q_PROPERTY(QString fontFamily MEMBER font_family_ NOTIFY styleChanged)
public:
    explicit PositionMap(QQuickItem* parent = nullptr);
    QVariantList pins() const { return pins_; }
    void setPins(const QVariantList& p);
    bool autoFit() const { return canvas_.auto_fit; }
    void setAutoFit(bool on);
    double spanKm() const { return canvas_.view.span_km; }
    int pinCount() const { return static_cast<int>(points_.size()); }
    Q_INVOKABLE void zoom(double factor);
    Q_INVOKABLE void pan(double dx_px, double dy_px);
    void paint(QPainter* p) override;

Q_SIGNALS:
    void pinsChanged();
    void viewChanged();
    void styleChanged();

protected:
    void geometryChange(const QRectF& n, const QRectF& o) override;

private:
    void step_fit();
    map::Style style() const { return {land_color_, coast_color_, line_color_, text_color_, dim_color_, QColor(), font_family_, false}; }
    QVariantList pins_;
    std::vector<map::Geo> points_;
    map::Canvas canvas_;
    QColor land_color_{0x1a, 0x1a, 0x1a}, coast_color_{0x4a, 0x4a, 0x4a}, line_color_{0x2c, 0x2c, 0x2c}, text_color_{0xd8, 0xd8, 0xd8},
           dim_color_{0x7c, 0x7c, 0x7c}, pin_color_{0xf0, 0x30, 0x30}, other_color_{0xf0, 0x90, 0x20};
    QString font_family_ = "Hack";
};

} // namespace spear::std_t99
