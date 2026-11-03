// 受信機の前段(App 側の段): radio.rx(4 Msps、LO = 選択チャネル − loOffset)の選択チャネルを DC に回し、100 kHz に落とす。
// Receiver は std-t99-receiver-sharp で試験信号を通した構成(入力 100 kHz、PFB 8 × 6.25 kHz)のまま使い、レート変換はここで行う。
// 原本は SDR# の任意の IQ レートを窓付き sinc の補間で 100 kHz にしていた。SPEAR では RF レートを App が決めるので、
// 整数比の FirDecimator 2 段(÷10 → 400 kHz、÷4 → 100 kHz)にする。
//   ÷10: 通過 ±180 kHz、127 tap(SLPR の resamp1 と同じ)。LO の DC スパイク(回転後 −loOffset)を落とす
//   ÷4 : 通過 ±40 kHz、63 tap(遷移 ~21 kHz)。100 kHz に折り返して Receiver の帯域(±25 kHz)に入る 75 kHz 以遠は阻止域
// provenance: 100 kHz の index → radio.rx の index(群遅延を引く。dsp/stage.hpp)。
#pragma once

#include "spear/core/types.hpp"
#include "spear/dsp/fir.hpp"
#include "spear/dsp/stage.hpp"

#include <complex>
#include <memory>
#include <span>
#include <vector>

namespace spear::std_t99 {

inline constexpr double kReceiverRate = 100e3;   // Receiver の入力(PFB 8 × 6.25 kHz の 2 倍)

class Front {
public:
    // in_rate: 100 kHz の整数倍(4 Msps を想定。100 kHz なら素通し)。shift_hz: 選択チャネル − 実際の LO(この量だけ下へ回す)
    Front(double in_rate, double shift_hz);
    // in を処理して band(400 kHz、無ければ空)と out(100 kHz)を返す。ブロック境界をまたいで位相連続
    void process(std::span<const cf32> in, std::vector<cf32>& band, std::vector<cf32>& out);
    // 100 kHz の index(この Front が出した累計)→ 入力の index(この Front が受け取った累計、実数)
    double input_index(double out_index) const { return prov_.input_index(out_index); }
    double band_rate() const { return band_rate_; }
    double shift_hz() const { return shift_hz_; }

private:
    double in_rate_, shift_hz_, band_rate_ = 0;
    std::unique_ptr<dsp::FirDecimator<cf32>> d1_, d2_;
    dsp::Provenance prov_ = dsp::Provenance::identity();
    std::complex<double> phasor_{1.0, 0.0}, step_{1.0, 0.0};
    std::vector<cf32> rot_, mid_;
};

} // namespace spear::std_t99
