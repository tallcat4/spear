// S.P.E.A.R. core — 個体の LO 誤差の換算(純関数、UHD 非依存)
//
// B2xx は RF LO も ADC クロックも同じ基準発振器(40 MHz TCXO)から作るので、誤差は周波数に比例する(ppm)。
// 符号は「観測周波数 − 真の周波数」: 信号が +p·f 高く見える ⇔ 装置の実 LO が p·f 低い。
// Radio は App が宣言した真の周波数を to_device() で装置の目盛りに換算して tune し、get_rx_freq() の読み値を
// to_true() で真の周波数に戻す。Radio の外(RfConfig / config() / event / StreamMeta / SigMF)はすべて真の周波数。
// 個体値は ~/spear/site.conf の radio.freq_err_ppm(コードにも要件にも埋めない)。
#pragma once

namespace spear {

struct LoCorrection {
    double ppm = 0;   // 観測 − 真 [ppm]。0 = 補正なし(Synthetic / Recording / 未設定)

    double factor() const { return 1.0 - ppm * 1e-6; }               // 実 LO / 要求 LO
    double to_device(double f_true) const { return f_true / factor(); }    // 真の周波数 → 装置に要求する値
    double to_true(double f_device) const { return f_device * factor(); }  // 装置の読み値 → 真の周波数
    double error_hz(double f_true) const { return f_true * ppm * 1e-6; }   // その周波数で信号が何 Hz ずれて見えるか
    bool active() const { return ppm != 0; }
};

} // namespace spear
