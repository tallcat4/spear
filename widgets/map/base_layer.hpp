// 地図の下地の描画(Qt Gui、QObject ではない。ADS-B と STD-T99 で共有)。docs/dsp-boundary.md の昇格ルールで ADS-B から抽出(2026-11-03)。
// 埋め込み地図(map_data)の陸 / 湖 / 国境 / 都道府県境 / 経緯度線 / 空港・滑走路 / 都市を、表示範囲(View)に合わせて画像に描く。
// 重ねて描くもの(機体、発信位置のピン)とタッチ操作は各 App の QQuickPaintedItem が持つ。
// 色は QML から Theme の値を渡す(計器の文法: 黒基調・フラット)。
#pragma once

#include "map_data.hpp"
#include "view.hpp"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QString>

namespace spear::map {

struct Style {
    QColor land{0x1a, 0x1a, 0x1a}, coast{0x4a, 0x4a, 0x4a}, line{0x2c, 0x2c, 0x2c}, text{0xd8, 0xd8, 0xd8}, dim{0x7c, 0x7c, 0x7c}, airport{0x40, 0xc0, 0xe0};
    QString font_family = "Hack";
    bool airports = true;   // 空港・滑走路を描く(ADS-B)
};

// 下地を w × h の画像に描く
QImage render_base(const View& view, int w, int h, const Style& style);
// 縮尺(左下)
void draw_scale_bar(QPainter& p, const View& view, int w, int h, const Style& style);

// 表示範囲の状態: 現在の View、自動フィットの目標、下地のキャッシュ。QQuickPaintedItem から使う
class Canvas {
public:
    View view;
    FitConfig fit;
    bool auto_fit = true;
    // 自動フィットを 1 段進める(pts は表示に含めたい点)。戻り値 = 表示が変わった
    bool step_fit(const std::vector<Geo>& pts, double aspect);
    bool set_auto_fit(bool on, const std::vector<Geo>& pts, double aspect);   // ON にしたときは即座に合わせる
    void zoom(double factor);
    void pan(double dx_px, double dy_px, double width_px);
    void resize(double w, double h) { view.aspect = h > 0 ? w / h : 1.0; dirty_ = true; }
    // 下地(表示範囲が変わったときだけ描き直す)
    const QImage& base(int w, int h, const Style& style);
    void invalidate() { dirty_ = true; }

private:
    QImage base_;
    bool dirty_ = true;
};

} // namespace spear::map
