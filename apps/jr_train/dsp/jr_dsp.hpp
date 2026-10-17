// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// JR 在来線デジタル列車無線の受信 DSP(1 チャネル分)。jr-conventional-line-radio-receiver の
// crates/jr-conventional-dsp(v1.1.0、commit dfc7440)を C++ に書き直したもの。アルゴリズム・定数・演算順は原本のまま。
//
//   入力 IQ(9.6 k〜20 MS/s)→ チャネルオフセットを NCO で DC へ(double)→ CIC(rate/256k の整数倍のときだけ)
//   → 32 相 polyphase で 19.2 kS/s(4 sample/symbol)→ AGC(64 sample のピーク + 約 1 s 減衰の基準、8% 未満は「不確か」)
//   → RRC α=0.2 の整合フィルタ + 微分 RRC(半シンボル間隔 21 点の窓)→ タイミング PI ループ
//   → π/4 交番の除去 + 2 次の搬送波ループ → 64 状態の適応系列推定(PSP-MLSE)→ 差動復号した dibit(0..3)
//
// 変調は 4800 baud の π/4-DQPSK。原本の Rust と同じ入力に同じ dibit を出すこと(tests/ の参照ベクトル)が仕様。
// 浮動小数の演算順を変えないこと(FMA 縮約も禁止: CMake で -ffp-contract=off)。
// Qt / UHD / spear_core に依存しない(Mac でも単体でビルド・テストできるように)。
#pragma once

#include <complex>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace spear::jr_train::dsp {

inline constexpr double kSymbolRate = 4800.0;
inline constexpr double kWorkRate = 19200.0;       // 4 sample/symbol
inline constexpr std::size_t kSignalCapacity = 1024; // コンスタレーション表示の最大点数

// 原本 Snapshot(C ABI の構造体と同じ並び)
struct Snapshot {
    int64_t symbols = 0;              // 出力した dibit 数
    int64_t locked_symbols = 0;       // そのうち搬送波ロック中のもの
    double carrier_error = 0;         // 系列推定の誤差振幅の平滑値(< 0.4 でロック)
    double frequency_offset_hz = 0;   // 搬送波ループの周波数推定 [Hz]
    int32_t carrier_locked = 0;
    int32_t reserved = 0;
};

class Demodulator {
public:
    // rate: 入力 IQ の sample rate [Hz]、offset: チャネル周波数 − IQ 中心 [Hz]。
    // profile 0 = 通常、1 = 診断用(I/Q 反転 + dibit 置換、原本の profile 1)。
    // 範囲外(rate 9.6 k〜20 M、|offset| + 4200 < rate/2、profile 0/1)は std::invalid_argument。
    Demodulator(double rate, double offset_hz, int profile = 0);
    ~Demodulator();
    Demodulator(Demodulator&&) noexcept;
    Demodulator& operator=(Demodulator&&) noexcept;

    // iq を処理し、出力 dibit を out の末尾に追加する。戻り値 = 追加した数。
    // 入力の区切り方に依らず同じ dibit 列になる(原本の性質)。非有限値を含む入力は状態を進めずに std::invalid_argument。
    std::size_t process(std::span<const std::complex<float>> iq, std::vector<uint8_t>& out);

    Snapshot snapshot() const;
    // 搬送波・タイミング・生き残り系列の伝送路で補正した差動シンボル(新しい窓、古い順)と EVM [%]。
    // 判定値ではなく受信のばらつきを残す。EVM は未校正の調整用指標。最大 kSignalCapacity 点。DSP の状態は変えない。
    double read_signal(std::vector<std::complex<float>>& points, std::size_t capacity = kSignalCapacity) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 引数の検査だけ(原本 receiver::Dsp::new と同じ条件)。問題なければ空文字列。
std::string validate(double rate, double offset_hz);

} // namespace spear::jr_train::dsp
