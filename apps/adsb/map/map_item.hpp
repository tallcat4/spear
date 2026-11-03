// ミニマップ(QQuickPaintedItem)。埋め込み地図(map_data)の上に航空機を描く。
// 表示範囲は自動フィット(位置のある全機を含む)。パン / ズームで手動になり、FIT で戻す。北が上。
// 地図(陸 / 湖 / 境界 / 都市 / 空港 / 滑走路)は表示範囲が変わったときだけ画像に描き直し、航空機はその上に毎回描く。
// 地図データ・表示範囲・下地の描画は widgets/map(spear::map::Canvas、STD-T99 と共有)。ここは機体と航跡、タッチの入口だけ。
// 色は QML から Theme の値を渡す(計器の文法: 黒基調・フラット)。
#pragma once

#include "map/base_layer.hpp"

#include <QColor>
#include <QImage>
#include <QQuickPaintedItem>
#include <QVariantList>

#include <deque>
#include <unordered_map>

namespace spear::adsb {

class MapItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList aircraft READ aircraft WRITE setAircraft NOTIFY aircraftChanged)
    Q_PROPERTY(int selectedIcao READ selectedIcao WRITE setSelectedIcao NOTIFY selectedChanged)
    Q_PROPERTY(bool autoFit READ autoFit WRITE setAutoFit NOTIFY viewChanged)
    Q_PROPERTY(double spanKm READ spanKm NOTIFY viewChanged)
    Q_PROPERTY(double centerLat READ centerLat NOTIFY viewChanged)
    Q_PROPERTY(double centerLon READ centerLon NOTIFY viewChanged)
    Q_PROPERTY(int positioned READ positioned NOTIFY aircraftChanged)
    Q_PROPERTY(bool mapLoaded READ mapLoaded CONSTANT)
    Q_PROPERTY(QColor landColor MEMBER land_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor coastColor MEMBER coast_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor lineColor MEMBER line_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER text_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor dimColor MEMBER dim_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor aircraftColor MEMBER aircraft_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor selectedColor MEMBER selected_color_ NOTIFY styleChanged)
    Q_PROPERTY(QColor airportColor MEMBER airport_color_ NOTIFY styleChanged)
    Q_PROPERTY(QString fontFamily MEMBER font_family_ NOTIFY styleChanged)
public:
    explicit MapItem(QQuickItem* parent = nullptr);

    QVariantList aircraft() const { return aircraft_; }
    void setAircraft(const QVariantList& a);
    int selectedIcao() const { return selected_; }
    void setSelectedIcao(int icao) { if (icao == selected_) return; selected_ = icao; Q_EMIT selectedChanged(); update(); }
    bool autoFit() const { return canvas_.auto_fit; }
    void setAutoFit(bool on);
    double spanKm() const { return canvas_.view.span_km; }
    double centerLat() const { return canvas_.view.center_lat; }
    double centerLon() const { return canvas_.view.center_lon; }
    int positioned() const { return static_cast<int>(points_.size()); }
    bool mapLoaded() const { return !map::world().land.empty(); }

    Q_INVOKABLE void zoom(double factor);                 // 中心を保って幅を factor 倍
    Q_INVOKABLE void pan(double dx_px, double dy_px);     // 画面ピクセルぶん動かす
    Q_INVOKABLE int icaoAt(double x, double y) const;     // タップ位置に最も近い機(半径 24 px 以内、無ければ 0)

    void paint(QPainter* p) override;

Q_SIGNALS:
    void aircraftChanged();
    void selectedChanged();
    void viewChanged();
    void styleChanged();

protected:
    void geometryChange(const QRectF& n, const QRectF& o) override;

private:
    void step_fit();

    QVariantList aircraft_;
    std::vector<map::Geo> points_;
    std::unordered_map<int, std::deque<map::Geo>> trails_;   // ICAO → 直近の位置
    int selected_ = 0;
    map::Canvas canvas_;
    QColor land_color_{0x1a, 0x1a, 0x1a}, coast_color_{0x4a, 0x4a, 0x4a}, line_color_{0x2c, 0x2c, 0x2c}, text_color_{0xd8, 0xd8, 0xd8},
           dim_color_{0x7c, 0x7c, 0x7c}, aircraft_color_{0x38, 0xd0, 0x38}, selected_color_{0xf0, 0xb0, 0x20}, airport_color_{0x40, 0xc0, 0xe0};
    QString font_family_ = "Hack";
    map::Style style() const { return {land_color_, coast_color_, line_color_, text_color_, dim_color_, airport_color_, font_family_, true}; }
};

} // namespace spear::adsb
