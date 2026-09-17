// S.P.E.A.R. core — SyntheticSource (要件 §7): 合成信号。ハードウェアなしの CI の土台 (§12.3)。
#pragma once

#include "source.hpp"

#include <random>
#include <vector>

namespace spear {

struct SyntheticSignal {
    struct Tone { double offset_hz; double amplitude; }; // amplitude: full scale = 1.0
    std::vector<Tone> tones;
    // 変調済みテスト信号(検証基盤。docs/dsp-boundary.md: 共通 DSP ではなくテスト用)
    struct Fm { double offset_hz = 0; double amplitude = 0; double deviation_hz = 0; double tone_hz = 0; bool enabled = false; };
    Fm fm;
    struct Am { double offset_hz = 0; double amplitude = 0; double depth = 0.5; double tone_hz = 0; bool enabled = false; };
    Am am;
    double noise_amplitude = 0.0;   // ガウス雑音の標準偏差 (full scale 比)
    uint64_t max_samples = 0;       // 0 = 無限。到達したら EOS
    uint32_t seed = 1;
};

class SyntheticSource final : public ThreadedSource {
public:
    SyntheticSource(EventBus* events, SyntheticSignal sig, std::size_t block_samples = kDefaultBlockSamples);
    void set_signal(SyntheticSignal sig); // stop 中のみ
protected:
    uint32_t fill(BlockBuilder& b) override;
    void on_start() override;
private:
    SyntheticSignal sig_;
    std::vector<double> phase_;
    double fm_phase_ = 0, fm_mod_phase_ = 0, am_phase_ = 0, am_mod_phase_ = 0;
    std::mt19937 rng_;
    uint64_t produced_ = 0;
};

} // namespace spear
