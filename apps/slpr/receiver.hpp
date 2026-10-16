// 特定小電力 20ch 受信機 — 純 C++(Qt / UHD / Stream Bus 非依存)。
//
// radio.rx(in_rate = pfb_channels × 12.5 kHz の整数倍、既定 4 Msps)の cf32 を受け取り、20 チャネルを並列に NFM 復調して音声を出す。
//   in → 回転(帯域中心を DC へ)→ resamp1(÷10 → 400 kHz)→ PFB 32ch(12.5 kHz/ch)→ [20ch] quadrature demod(±2.5 kHz → ±1.0)
//   → 音声 LPF(3 kHz)→ DC 阻止 → squelch のゲート → 音声 12.5 kHz
// 構成は STD-T98 の受信機と同じ骨格(resamp1 → PFB → チャネルごと)。デジタルでも秘話でもないので、シンボル同期以降が無い。
//
// squelch は STD-T98 と同じ床相対(閾値 = そのブロックの全チャネル電力の中央値 + margin、ヒステリシス、hold)。絶対 dB の閾値は
// AGC で床が動くと空きチャネルが開閉する(2026-09-20 の実地、docs/apps/std_t98.md「スケルチ」)。閉でも復調は回し、音声の出力だけを 0 にする。
//
// 観測点(§1.1): Observer の各 hook が全チャネルの中間 stream を受け取る。App はそれを Stream Bus に publish / ミックスするだけ。
#pragma once

#include "channels.hpp"
#include "spear/core/types.hpp"
#include "spear/dsp/channelizer.hpp"
#include "spear/dsp/fir.hpp"
#include "spear/dsp/stage.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace spear::slpr {

struct ReceiverConfig {
    double in_rate = 4e6;            // radio.rx(400 kHz の整数倍)
    int    pfb_channels = 32;        // × 12.5 kHz = 400 kHz(20 ch は ±125 kHz = bin −10..+10 に収まる)
    double freq_err_hz = 0;          // 入力を −freq_err 回転する量 = 帯域中心 − 実 LO(個体の LO 誤差は Core が打ち消すので App は足さない)
    // 用途の定数(特定小電力の狭帯域 FM: 12.5 kHz 間隔、最大周波数偏移 ±2.5 kHz、音声 300–3000 Hz)。共通 DSP には置かない
    double fm_dev_hz = 2500;         // 偏移 ±2.5 kHz → 音声 ±1.0
    double audio_cutoff_hz = 3000;   // 音声 LPF(12.5 kHz で 63 tap)
    double audio_hp_hz = 200;        // 一次の DC 阻止(送信機の周波数ずれ = 検波後の直流を除く。CTCSS 等のサブ可聴音もここで薄まる)
    // squelch: 閾値 = 床(全 ch 電力の中央値)+ margin。開くのは閾値を attack_s 超え続けたとき(1 ブロックだけの過渡 — 隣の局のキーアップ、
    // インパルス雑音 — で 150 ms の雑音を鳴らさない。無線機のスケルチ立ち上がりも 10–50 ms)。閉じるのは 床 + margin − hyst を hold_s
    // 下回り続けたとき(音声の息継ぎで切らない)
    double squelch_margin_db = 8, squelch_hyst_db = 3, squelch_attack_s = 0.02, squelch_hold_s = 0.15;
};

struct ChannelMetrics {
    double power_db = -999;          // 直近ブロックのチャネル電力
    double floor_db = -999;          // 直近ブロックの床(全チャネル共通)
    double threshold_db = -999;      // 開く閾値 = floor + margin
    bool   open = false;             // squelch
    double audio_db = -999;          // 直近ブロックのゲート後音声(±1.0 基準)
    uint64_t opens = 0;              // 開いた回数
    double open_s = 0;               // 開いていた合計時間 [s]
};

// 中間 stream の観測。全チャネル分呼ばれる。first_index はそのチャネルの 12.5 kHz sample の通し番号(全チャネル同じ歩み)。
struct Observer {
    std::function<void(std::span<const cf32> iq, uint64_t first_index)> band;                     // resamp1 後(400 kHz)、20ch 俯瞰用
    std::function<void(int ch, std::span<const cf32> iq, uint64_t first_index)> channel_iq;        // 12.5 kHz(PFB 出力)
    std::function<void(int ch, std::span<const float> disc)> discriminator;                        // 12.5 kHz、±1.0 = ±fm_dev
    std::function<void(int ch, std::span<const float> audio, uint64_t first_index)> audio;         // 12.5 kHz、LPF・DC 阻止・ゲート後
    std::function<void(int ch, bool open, uint64_t input_index)> squelch;                          // 開閉(provenance: radio.rx の index)
};

class Receiver {
public:
    explicit Receiver(ReceiverConfig cfg);
    ~Receiver();
    const ReceiverConfig& config() const { return cfg_; }
    int channel_to_bin(int ch) const;                  // PFB の bin(負のオフセットは上位 bin)
    void set_squelch_margin_db(double db) { cfg_.squelch_margin_db = db; }
    void set_observer(Observer o) { obs_ = std::move(o); }

    // 入力ブロック(cfg.in_rate)。first_index = radio.rx の sample index(provenance)
    void process(std::span<const cf32> in, uint64_t first_index);
    const ChannelMetrics& metrics(int ch) const { return metrics_[static_cast<std::size_t>(ch)]; }
    double band_rate() const { return cfg_.pfb_channels * kSpacingHz; }
    double audio_rate() const { return kSpacingHz; }
    const dsp::Provenance& provenance_to_channel() const { return prov_chan_; }   // 12.5 kHz sample → in index

private:
    struct Channel;
    ReceiverConfig cfg_;
    Observer obs_;
    std::unique_ptr<dsp::FirDecimator<cf32>> resamp1_;
    std::unique_ptr<dsp::PfbChannelizer> pfb_;
    std::vector<std::unique_ptr<Channel>> ch_;
    std::vector<ChannelMetrics> metrics_;
    dsp::Provenance prov_chan_;
    std::vector<cf32> rot_;               // 回転後の入力
    double rot_phase_ = 0;
    std::vector<cf32> buf1_;
    std::vector<std::vector<cf32>> bins_;
    std::vector<double> floor_tmp_;       // 床(中央値)計算用
    uint64_t band_index_ = 0;   // resamp1 出力の通し番号
    uint64_t chan_index_ = 0;   // 12.5 kHz sample の通し番号(全チャネル共通)
    uint64_t in0_ = 0;          // 最初のブロックの radio.rx index(provenance の原点)
    bool have_in0_ = false;
};

} // namespace spear::slpr
