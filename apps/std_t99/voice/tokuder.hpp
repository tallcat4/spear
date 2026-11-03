// TOKUDER (ARIB STD-T99 Annex 2 voice) frames -> 8 kHz speech.
// C++ port of tokuder-neurodec (https://github.com/tallcat4/tokuder-neurodec, GPL-3.0-or-later):
//  - fec: the TCH channel code recovered by tokuder-neurodec (2 x extended Golay [24,12] + 24 uncoded bits,
//    interleaved by 3), soft maximum-likelihood decoding -> 48 voice bits (fec.py info48)
//  - Decoder: model tokuder48_causal_v1 (bit-token MLP frontend + causal ConvNeXt blocks, one frame look-ahead,
//    harmonic-plus-noise excitation, causal iSTFT generator). Every layer is causal, so the decoder runs frame
//    by frame with state and gives the same samples as the batch PyTorch model (model.py decode) apart from the
//    excitation noise and the float32 phase accumulation.
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace spear::std_t99::tokuder {

inline constexpr int kFrameSamples = 160;   // 20 ms at 8 kHz
inline constexpr int kInfoBits = 48;

// 72 LLRs (positive = 1, transmission order) -> 48 voice bits; returns corrected bits of both Golay words
int soft_decode(const int8_t* llr72, uint8_t* info48);

class Weights {
public:
    Weights(const uint8_t* blob, std::size_t size);
    const float* get(const std::string& name, std::initializer_list<int> shape) const;
private:
    struct Entry { std::vector<int> shape; const float* data; };
    std::map<std::string, Entry> entries_;
    std::vector<float> storage_;   // aligned copy of the blob payload
};
const Weights& model();            // the embedded tokuder48_causal_v1

class Decoder {
public:
    explicit Decoder(const Weights& w);
    ~Decoder();
    // One 20 ms frame (48 info bits, frame 0 = TCH slot 0 of an RF frame). Appends the samples that became final.
    void push(const uint8_t* info48, std::vector<float>& pcm);
    // Ends the stream: appends the remaining samples and resets.
    void flush(std::vector<float>& pcm);
    void reset();
    uint64_t frames() const { return pushed_; }
    // Tests: excitation noise source (nullptr = internal Gaussian generator)
    void set_noise(const float* noise, std::size_t count) { noise_ = noise; noise_count_ = count; noise_used_ = 0; }
    // Tests: frontend outputs per frame (80 log-mel + 7 periodicity head), available one frame late
    std::vector<float> probe;
    bool keep_probe = false;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    uint64_t pushed_ = 0;
    const float* noise_ = nullptr;
    std::size_t noise_count_ = 0, noise_used_ = 0;
    float noise();
    std::mt19937 rng_{12345};
    std::normal_distribution<float> gauss_{0.f, 1.f};
    void emit(const float* features, std::vector<float>& pcm);   // features of frame f -> two generator steps
    void step(const float* cond, const float* exc, std::vector<float>& pcm);
    void output(std::vector<float>& pcm, uint64_t until);
};

} // namespace spear::std_t99::tokuder
