// ADS-B / Mode S 受信機 — 純 C++(Qt / UHD / Stream Bus 非依存)。
//
// radio.rx(in_rate = 2 MHz の整数倍、LO = 1090 MHz − lo_offset)の cf32 を受け取り、フレームを出す。
//   in → 回転(−lo_offset: 1090 MHz を 0 Hz へ)→ 低域 FIR(±cutoff、雑音帯域を絞る)→ |x|² → PpmDecoder
//   → CRC-24 → (DF17/18 は任意で 1 bit 訂正)→ Message(protocol/modes)
// 振幅は回転に不変なので周波数同期は要らず、B210 個体の LO 誤差(kHz 級)は復号に影響しない。
// AP 形式(DF0/4/5/16/20/21)のアドレスは直近 icao_cache_s 秒以内に DF11/17/18 で見た ICAO と一致するときだけ受理する(dump1090 と同じ)。
#pragma once

#include "dsp/ppm.hpp"
#include "protocol/modes.hpp"
#include "spear/core/types.hpp"
#include "spear/dsp/fir.hpp"
#include "spear/dsp/stage.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace spear::adsb {

struct ReceiverConfig {
    double in_rate = 8e6;            // radio.rx(2 MHz の整数倍)
    double lo_offset_hz = 2e6;       // LO と 1090 MHz の差(受信信号を −lo_offset 回転して 0 Hz へ)
    double lowpass_cutoff_hz = 1.5e6;
    int    lowpass_taps = 47;
    double preamble_ratio_db = 6;    // プリアンブルのパルス / 無音(電力比)。実録音(羽田近傍 30 s)で 9.5 dB → 847 フレーム、6 dB → 1223、3 dB → 1239(候補 3.9M で CPU 過多)
    double min_pulse_db = -8;        // 各パルス / パルス平均 の下限
    bool   fix_single_bit = true;    // DF17/18 の 1 bit 誤り訂正(訂正したフレームは既知 ICAO のときだけ受理: 誤訂正で幻の機体を作らない)
    double icao_cache_s = 300;       // AP 形式の照合に使う既知 ICAO の寿命
};

struct Frame {
    uint64_t input_sample_index = 0; // radio.rx の index に逆算したプリアンブル先頭(provenance §4.5)
    uint64_t input_sample_end = 0;   // フレーム末尾(exclusive)
    double   t_s = 0;                // 受信機の時間軸(入力 sample / in_rate)
    FrameBytes bytes{};
    int      nbits = 56;
    Message  msg;
    bool     accepted = false;       // CRC OK(DF11/17/18)または既知 ICAO に一致(AP 形式)
    bool     fixed = false;          // 1 bit 訂正で通った
    double   rssi_db = -100;         // パルスの平均電力 [dBFS]
    double   snr_db = 0;             // パルス側 / 無音側 [dB]
};

struct Metrics {
    uint64_t preambles = 0;          // プリアンブル候補
    uint64_t frames = 0;             // 受理したフレーム
    uint64_t crc_bad = 0;            // 長さ 112 で CRC 不良(訂正も不可)
    uint64_t fixed = 0;
    uint64_t ap_unknown = 0;         // AP 形式でアドレス未知(捨てた)
    uint64_t by_df[32] = {};         // 受理フレームの DF 別
    std::size_t known_icao = 0;
};

struct Observer {
    std::function<void(std::span<const float> pwr, uint64_t first_index)> power;   // FIR 後の電力列(TAP 用)
    std::function<void(const Frame&, std::span<const float> window)> frame;        // 受理フレーム + その電力窓(120 µs)
};

class Receiver {
public:
    explicit Receiver(ReceiverConfig cfg);
    const ReceiverConfig& config() const { return cfg_; }
    void set_observer(Observer o) { obs_ = std::move(o); }
    void set_fix_single_bit(bool on) { cfg_.fix_single_bit = on; }
    // 入力ブロック(cfg.in_rate)。first_index = radio.rx の sample index(provenance)
    void process(std::span<const cf32> in, uint64_t first_index);
    const Metrics& metrics() const { return metrics_; }
    int samples_per_chip() const { return ppm_->config().spc; }
    const dsp::Provenance& provenance() const { return prov_; }   // 電力列 index → in index
    double now_s() const { return static_cast<double>(mag_index_) / cfg_.in_rate; }   // 受信機の時間軸(Frame::t_s と同じ)

private:
    bool accept(const Candidate& c);
    ReceiverConfig cfg_;
    Observer obs_;
    std::unique_ptr<dsp::FirDecimator<cf32>> lowpass_;
    std::unique_ptr<PpmDecoder> ppm_;
    dsp::Provenance prov_;
    std::array<uint32_t, 112> syndromes_{};
    std::vector<cf32> rot_, filt_;
    std::vector<float> pwr_;
    cf32 phasor_{1.f, 0.f}, step_{1.f, 0.f};
    uint64_t mag_index_ = 0;        // 電力列の通し番号
    int64_t  in_offset_ = 0;        // radio.rx index − 電力列 index(不連続・generation 切替で取り直す)
    bool have_offset_ = false;
    std::unordered_map<uint32_t, double> known_icao_;   // ICAO → 最後に見た t_s
    double last_prune_s_ = 0;
    Metrics metrics_;
};

} // namespace spear::adsb
