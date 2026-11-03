// One-channel STD-T99 receiver. The DSP chain is the one used by std-t98-receiver
// (SPEAR stages): rotate the selected channel to DC, decimate to the PFB band rate,
// extract bin 0, interpolate x10 to 62.5 kHz, FM discriminator, RX filter H/P (alpha 0.2),
// Gardner symbol sync and the SPEAR sync-word state machine (DC-tolerant sync metric, frame_sync.hpp). Both standards use
// 4.8 kbps 4-level FSK (±315/±945 Hz), the same sync word CDF59h and 192-symbol frames.
#pragma once

#include "dsp/center_channelizer.hpp"
#include "protocol/protocol.hpp"
#include "spear/core/types.hpp"
#include "spear/dsp/fir.hpp"
#include "spear/dsp/stage.hpp"
#include "std_t98/dsp/symbol_sync.hpp"
#include "dsp/frame_sync.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace spear::std_t99 {

struct ReceiverConfig {
    double in_rate = 1.2e6;
    int    pfb_channels = 64;          // band rate = pfb_channels * spacing
    double spacing_hz = 6250;
    int    resamp2 = 10;               // 6250 -> 62500
    double baud = 2400;
    double fsk_dev_hz = 315;           // per level (Table Annex 2-3-2)
    double freq_err_hz = 0;            // input is rotated by -freq_err_hz (channel offset + residual)
    double sync_ratio = 0.2, sync_locked_ratio = 0.6;
    int    sync_max_misses = 3;
    double sym_loop_bw = 0.06, sym_damping = 1.1, sym_ted_gain = 0.1, sym_max_dev = 0.02;
    float  post_filt_gain = 0.23f, post_sync_gain = 5.f;
};

enum class HalfKind { tch, facch, pich };

struct Half {
    HalfKind kind = HalfKind::tch;
    Bits bits;                          // 144 dewhitened bits
    std::optional<Control> control;     // PICH / FACCH
    std::array<uint8_t, 18> payload{};  // TCH: two 72-bit voice frames
    std::array<int8_t, 144> llr{};
};

struct Frame {
    uint64_t symbol_index = 0;
    uint64_t input_sample_index = 0;    // first sync-word symbol, in input samples
    double   sync_sse = 0;
    bool     sync_ok = true;            // false = flywheel frame confirmed by CRC or the next frame
    std::vector<float> raw;             // 192 soft symbols before quantization
    FrameFields fields;                 // dewhitened
    Rich  rich;
    Sacch sacch;
    std::array<Half, 2> halves;
    bool sync_burst() const { return rich.f == 0b01; }
    bool voice() const { return rich.f == 0b10 && halves[0].kind == HalfKind::tch && halves[1].kind == HalfKind::tch; }
    bool crc_ok() const;                // any CRC of SACCH / PICH / FACCH
    bool control_crc_ok() const;        // CRC-12 of PICH / FACCH
    // RICH holds a combination a transmitter can send (Table Annex 2-4-7), or a CRC-12 vouches for the frame.
    // Noise after a transmission passes CRC-6 about once in 64 frames but rarely gives a valid RICH as well.
    bool plausible() const;
};

// Decode one 192-symbol candidate (exposed for tests and spec vectors)
Frame decode_frame(std::span<const float> raw192);

struct ChannelMetrics {
    double power_db = -999;
    bool   locked = false;
    uint64_t frames = 0, crc_frames = 0, locks = 0, flywheel_frames = 0, dropped_frames = 0;
    double sps = 0;
    double freq_offset_hz = 0;          // SPEAR: 同期語の DC × 偏移(搬送波のずれ、表示用)
    bool   have_offset = false;
};

class Receiver {
public:
    explicit Receiver(ReceiverConfig cfg);
    ~Receiver();
    void set_frame_handler(std::function<void(const Frame&)> f) { on_frame_ = std::move(f); }
    // std-t99-receiver-sharp: eye-pattern traces (eye_points samples spanning two symbols, centred on a decision point)
    void set_eye_handler(std::function<void(std::span<const float>)> f, int points = 64) { on_eye_ = std::move(f); eye_points_ = points; }
    void process(std::span<const spear::cf32> in, uint64_t first_index);
    const ChannelMetrics& metrics() const { return metrics_; }
    const ReceiverConfig& config() const { return cfg_; }

private:
    ReceiverConfig cfg_;
    std::function<void(const Frame&)> on_frame_;
    std::function<void(std::span<const float>)> on_eye_;
    int eye_points_ = 64;
    std::vector<float> eye_buf_, eye_trace_;
    std::vector<double> eye_pending_;
    uint64_t eye_base_ = 0, eye_fed_ = 0;
    void eye(std::size_t n3);
    ChannelMetrics metrics_;
    std::unique_ptr<spear::dsp::FirDecimator<spear::cf32>> resamp1_;
    std::unique_ptr<CenterChannelizer> pfb_;
    spear::dsp::FirInterpolator<spear::cf32> up_;
    spear::dsp::FirDecimator<float> rrc_;
    spear::std_t98::SymbolSync sync_;
    SyncCorrelator corr_;
    spear::dsp::Provenance prov_;
    std::vector<spear::cf32> rot_, buf1_, iq_up_;
    std::vector<std::vector<spear::cf32>> bins_;
    std::vector<float> disc_, filt_, syms_;
    std::vector<double> sym_pos_;
    std::vector<std::pair<uint64_t, double>> pos_hist_;
    std::vector<Frame> pending_;
    spear::cf32 quad_prev_{0.f, 0.f};
    float quad_gain_ = 1.f;
    float frame_dc_ = 0.f;
    double rot_phase_ = 0;
    uint64_t symbol_index_ = 0, in0_ = 0;
    bool have_in0_ = false;

    // Acquisition: the Gardner loop may still be pulling in when a transmission starts, which loses the
    // synchronous burst (identity, position). acquire() searches the sync word directly on the filtered
    // 62.5 kHz signal at every sample phase and decodes the frame there; only frames that pass a 12-bit
    // CRC (PICH/FACCH) are accepted, and while locked only synchronous bursts. emit() drops duplicates.
    std::vector<float> acq_;                 // filtered samples in symbol units
    uint64_t acq_base_ = 0;                  // absolute filtered-sample index of acq_[0]
    double acq_next_ = 0;                    // next position to test
    double acq_follow_ = -1;                 // std-t99-receiver-sharp: predicted start of the frame after an acquired one
    std::vector<uint64_t> recent_;           // input_sample_index of recently emitted frames
    uint64_t grid_ = 0;                      // std-t99-receiver-sharp: input_sample_index of the last frame with a CRC
    bool have_grid_ = false;
    void acquire();
    void emit(const Frame& f);
    static int score(const Frame& f);                                   // number of CRCs that pass
    float acq_at(double x) const;                                       // interpolated filtered sample
    double sync_metric(double position, float& dc) const;               // DC-free sync-word SSE
    bool decode_at(double position, Frame& f, double* best_position = nullptr) const;   // best phase within ±1/2 symbol
    uint64_t input_index(double filtered_sample) const;
};

} // namespace spear::std_t99
