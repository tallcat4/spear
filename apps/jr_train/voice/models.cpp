// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/models.rs、src/model_bindings/{binary,predictor,correction,planner}.rs、src/synthesis_models.rs、src/celp/config.rs
#include "models.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace spear::jr_train::voice {
namespace {

Blob require(const char* name) {
    const Blob b = embedded_model(name);
    if (!b.data) throw std::runtime_error(std::string("jr_voice: missing embedded model ") + name);
    return b;
}

// 原本 binary.rs の readers(位置合わせに依存しない LE 読み出し)
template <class T>
T read_le(const Blob& b, std::size_t offset) {
    if (offset + sizeof(T) > b.size) throw std::runtime_error("jr_voice: truncated model");
    T v;
    std::memcpy(&v, b.data + offset, sizeof(T));   // 対象はリトルエンディアンのみ(x86-64 / arm64)
    return v;
}
template <class T>
std::vector<T> array(const Blob& b, std::size_t offset, std::size_t n) {
    if (offset > b.size || n > (b.size - offset) / sizeof(T)) throw std::runtime_error("jr_voice: truncated model");
    std::vector<T> out(n);
    std::memcpy(out.data(), b.data + offset, n * sizeof(T));
    return out;
}
std::vector<float> floats(const Blob& b, std::size_t offset, std::size_t n) { return array<float>(b, offset, n); }
std::vector<std::size_t> indices(const Blob& b, std::size_t offset, std::size_t n) {
    const auto raw = array<uint32_t>(b, offset, n);
    return std::vector<std::size_t>(raw.begin(), raw.end());
}

Tensor tf(const std::vector<float>& d, std::size_t rows, std::size_t cols) {
    Tensor t;
    t.data = d.data();
    t.rows = rows;
    t.cols = cols;
    return t;
}
Tensor tq(const std::vector<int8_t>& d, std::size_t rows, std::size_t cols, float quantum, const std::vector<uint8_t>* rows_v = nullptr,
          const std::vector<uint16_t>* offsets = nullptr) {
    Tensor t;
    t.quantized = d.data();
    t.quantized_len = d.size();
    t.rows = rows;
    t.cols = cols;
    t.quantum = quantum;
    if (rows_v && offsets) {
        t.sparse_rows = rows_v->data();
        t.sparse_offsets = offsets->data();
    }
    return t;
}

} // namespace

// ---- predictor.bin ----
PredictorModel::PredictorModel() {
    const Blob d = require("predictor");
    baseline_bias = floats(d, 0, 17);
    baseline_weight = floats(d, 68, 4760);
    calibration_bias = floats(d, 19108, 32);
    calibration_mean = floats(d, 19236, 67);
    calibration_scale = floats(d, 19504, 67);
    calibration_weight = floats(d, 19772, 2144);
    context_layer_0_bias = floats(d, 28348, 64);
    context_layer_0_weight_q = array<int8_t>(d, 28604, 64448);
    context_layer_1_bias = floats(d, 93052, 32);
    context_layer_1_weight = floats(d, 93180, 2048);
    context_layer_2_bias = floats(d, 101372, 17);
    context_layer_2_weight = floats(d, 101440, 544);
    context_x_mean = floats(d, 103616, 1007);
    context_x_scale = floats(d, 107644, 1007);
    context_y_mean = floats(d, 111672, 17);
    context_y_scale = floats(d, 111740, 17);
    levels_layer_0_bias = floats(d, 111808, 32);
    levels_layer_0_weight = floats(d, 111936, 1632);
    levels_layer_1_bias = floats(d, 118464, 32);
    levels_layer_1_weight = floats(d, 118592, 1024);
    levels_layer_2_bias = floats(d, 122688, 17);
    levels_layer_2_weight = floats(d, 122756, 544);
    levels_x_mean = floats(d, 124932, 51);
    levels_x_scale = floats(d, 125136, 51);
    levels_y_mean = floats(d, 125340, 17);
    levels_y_scale = floats(d, 125408, 17);
    profile_weight = floats(d, 125476, 11067);
    recurrent_gate_bias = floats(d, 169744, 192);
    recurrent_input_weight_q = array<int8_t>(d, 170512, 3824);
    recurrent_input_weight_rows = array<uint8_t>(d, 174336, 3824);
    recurrent_input_weight_offsets = array<uint16_t>(d, 178160, 193);
    recurrent_output_bias = floats(d, 178546, 70);
    recurrent_output_weight_q = array<int8_t>(d, 178826, 4480);
    recurrent_recurrent_weight_q = array<int8_t>(d, 183306, 2249);
    recurrent_recurrent_weight_rows = array<uint8_t>(d, 185555, 2249);
    recurrent_recurrent_weight_offsets = array<uint16_t>(d, 187804, 193);
    recurrent_x_mean = floats(d, 188190, 81);
    recurrent_x_scale = floats(d, 188514, 81);
    recurrent_y_mean = floats(d, 188838, 70);
    recurrent_y_scale = floats(d, 189118, 70);
    refinement_pitch_bias = floats(d, 189398, 1);
    refinement_pitch_mean = floats(d, 189402, 37);
    refinement_pitch_scale = floats(d, 189550, 37);
    refinement_pitch_weight = floats(d, 189698, 37);
    refinement_spectral_bias = floats(d, 189846, 16);
    refinement_spectral_mean = floats(d, 189910, 36);
    refinement_spectral_scale = floats(d, 190054, 36);
    refinement_spectral_table = floats(d, 190198, 8192);
    refinement_spectral_weight = floats(d, 222966, 576);
    spectrum_layer_0_bias = floats(d, 225270, 64);
    spectrum_layer_0_weight_q = array<int8_t>(d, 225526, 64448);
    spectrum_layer_1_bias = floats(d, 289974, 32);
    spectrum_layer_1_weight = floats(d, 290102, 2048);
    spectrum_layer_2_bias = floats(d, 298294, 17);
    spectrum_layer_2_weight = floats(d, 298362, 544);
    spectrum_x_mean = floats(d, 300538, 1007);
    spectrum_x_scale = floats(d, 304566, 1007);
    spectrum_y_mean = floats(d, 308594, 17);
    spectrum_y_scale = floats(d, 308662, 17);
    voicing_bias = floats(d, 308730, 2);
    voicing_weight = floats(d, 308738, 168);
    voicing_x_mean = floats(d, 309410, 84);
    voicing_x_scale = floats(d, 309746, 84);
    context_blend = floats(d, 310082, 17);
    level_blend = floats(d, 310150, 17);
    spectral_blend = floats(d, 310218, 17);
    recurrent_blend = floats(d, 310286, 70);

    BASELINE_BIAS = tf(baseline_bias, 17, 1);
    BASELINE_WEIGHT = tf(baseline_weight, 280, 17);
    CALIBRATION_BIAS = tf(calibration_bias, 32, 1);
    CALIBRATION_MEAN = tf(calibration_mean, 67, 1);
    CALIBRATION_SCALE = tf(calibration_scale, 67, 1);
    CALIBRATION_WEIGHT = tf(calibration_weight, 67, 32);
    PROFILE_WEIGHT = tf(profile_weight, 651, 17);
    RECURRENT_GATE_BIAS = tf(recurrent_gate_bias, 192, 1);
    RECURRENT_INPUT_WEIGHT = tq(recurrent_input_weight_q, 81, 192, read_le<float>(d, 310854), &recurrent_input_weight_rows, &recurrent_input_weight_offsets);
    RECURRENT_OUTPUT_BIAS = tf(recurrent_output_bias, 70, 1);
    RECURRENT_OUTPUT_WEIGHT = tq(recurrent_output_weight_q, 64, 70, read_le<float>(d, 310858));
    // 原本どおり recurrent の尺度は output と同じ位置(310858)を読む
    RECURRENT_RECURRENT_WEIGHT = tq(recurrent_recurrent_weight_q, 64, 192, read_le<float>(d, 310858), &recurrent_recurrent_weight_rows, &recurrent_recurrent_weight_offsets);
    RECURRENT_X_MEAN = tf(recurrent_x_mean, 81, 1);
    RECURRENT_X_SCALE = tf(recurrent_x_scale, 81, 1);
    RECURRENT_Y_MEAN = tf(recurrent_y_mean, 70, 1);
    RECURRENT_Y_SCALE = tf(recurrent_y_scale, 70, 1);
    REFINEMENT_PITCH_BIAS = tf(refinement_pitch_bias, 1, 1);
    REFINEMENT_PITCH_MEAN = tf(refinement_pitch_mean, 37, 1);
    REFINEMENT_PITCH_SCALE = tf(refinement_pitch_scale, 37, 1);
    REFINEMENT_PITCH_WEIGHT = tf(refinement_pitch_weight, 37, 1);
    REFINEMENT_SPECTRAL_BIAS = tf(refinement_spectral_bias, 16, 1);
    REFINEMENT_SPECTRAL_MEAN = tf(refinement_spectral_mean, 36, 1);
    REFINEMENT_SPECTRAL_SCALE = tf(refinement_spectral_scale, 36, 1);
    REFINEMENT_SPECTRAL_TABLE = tf(refinement_spectral_table, 512, 16);
    REFINEMENT_SPECTRAL_WEIGHT = tf(refinement_spectral_weight, 36, 16);
    VOICING_BIAS = tf(voicing_bias, 2, 1);
    VOICING_WEIGHT = tf(voicing_weight, 84, 2);
    VOICING_X_MEAN = tf(voicing_x_mean, 84, 1);
    VOICING_X_SCALE = tf(voicing_x_scale, 84, 1);
    CONTEXT = Mlp{tf(context_x_mean, 1007, 1), tf(context_x_scale, 1007, 1), tf(context_y_mean, 17, 1), tf(context_y_scale, 17, 1),
                  {tq(context_layer_0_weight_q, 1007, 64, read_le<float>(d, 310850)), tf(context_layer_1_weight, 64, 32), tf(context_layer_2_weight, 32, 17)},
                  {tf(context_layer_0_bias, 64, 1), tf(context_layer_1_bias, 32, 1), tf(context_layer_2_bias, 17, 1)}};
    LEVELS = Mlp{tf(levels_x_mean, 51, 1), tf(levels_x_scale, 51, 1), tf(levels_y_mean, 17, 1), tf(levels_y_scale, 17, 1),
                 {tf(levels_layer_0_weight, 51, 32), tf(levels_layer_1_weight, 32, 32), tf(levels_layer_2_weight, 32, 17)},
                 {tf(levels_layer_0_bias, 32, 1), tf(levels_layer_1_bias, 32, 1), tf(levels_layer_2_bias, 17, 1)}};
    SPECTRUM = Mlp{tf(spectrum_x_mean, 1007, 1), tf(spectrum_x_scale, 1007, 1), tf(spectrum_y_mean, 17, 1), tf(spectrum_y_scale, 17, 1),
                   {tq(spectrum_layer_0_weight_q, 1007, 64, read_le<float>(d, 310862)), tf(spectrum_layer_1_weight, 64, 32), tf(spectrum_layer_2_weight, 32, 17)},
                   {tf(spectrum_layer_0_bias, 64, 1), tf(spectrum_layer_1_bias, 32, 1), tf(spectrum_layer_2_bias, 17, 1)}};
    PITCH_BLEND = read_le<float>(d, 310566);
    for (std::size_t r = 0; r < 7; ++r) {
        const auto row = indices(d, 310570 + r * 8 * 4, 8);
        for (std::size_t c = 0; c < 8; ++c) PROFILE_GROUPS[r][c] = row[c];
    }
    const auto lengths = indices(d, 310794, 7), offsets = indices(d, 310822, 7);
    for (std::size_t i = 0; i < 7; ++i) {
        PROFILE_LENGTHS[i] = lengths[i];
        PROFILE_OFFSETS[i] = offsets[i];
    }
}

const PredictorModel& predictor_model() {
    static const PredictorModel m;
    return m;
}

// ---- correction.bin ----
CorrectionModel::CorrectionModel() {
    const Blob d = require("correction");
    GAIN_MEAN = floats(d, 0, 12);
    GAIN_SCALE = floats(d, 48, 12);
    GAIN_WEIGHT = floats(d, 96, 141);
    LINEAR_MEAN = floats(d, 660, 36);
    LINEAR_SCALE = floats(d, 804, 36);
    linear_coefficient_q = array<int8_t>(d, 948, 8784);
    linear_coefficient_scale = floats(d, 9732, 16);
    MAIN_TABLE = array<int16_t>(d, 9796, 1280);
    STUDENT_X_MEAN = floats(d, 12356, 145);
    STUDENT_X_SCALE = floats(d, 12936, 145);
    STUDENT_Y_MEAN = floats(d, 13516, 16);
    STUDENT_Y_SCALE = floats(d, 13580, 16);
    student_layer_0_weight_q = array<int8_t>(d, 13644, 3480);
    student_layer_0_weight_scale = floats(d, 17124, 24);
    STUDENT_LAYER_0_BIAS = floats(d, 17220, 24);
    student_layer_1_weight_q = array<int8_t>(d, 17316, 288);
    student_layer_1_weight_scale = floats(d, 17604, 12);
    STUDENT_LAYER_1_BIAS = floats(d, 17652, 12);
    student_layer_2_weight_q = array<int8_t>(d, 17700, 192);
    student_layer_2_weight_scale = floats(d, 17892, 16);
    STUDENT_LAYER_2_BIAS = floats(d, 17956, 16);
    LINEAR_COEFFICIENT = {linear_coefficient_q.data(), linear_coefficient_scale.data(), 16};
    STUDENT_LAYER_0_WEIGHT = {student_layer_0_weight_q.data(), student_layer_0_weight_scale.data(), 24};
    STUDENT_LAYER_1_WEIGHT = {student_layer_1_weight_q.data(), student_layer_1_weight_scale.data(), 12};
    STUDENT_LAYER_2_WEIGHT = {student_layer_2_weight_q.data(), student_layer_2_weight_scale.data(), 16};
}

const CorrectionModel& correction_model() {
    static const CorrectionModel m;
    return m;
}

// ---- planner.bin ----
PlannerModel::PlannerModel() {
    const Blob d = require("planner");
    LSF_MEAN = floats(d, 0, 10);
    SPECTRAL = floats(d, 40, 1920);
    MA = floats(d, 7720, 80);
    GAIN_LOG = floats(d, 8040, 128);
    GAIN_ADAPTIVE = floats(d, 8552, 128);
    CLASS_BASE = array<uint16_t>(d, 9064, 6);
    INDEX_LIMIT = array<uint16_t>(d, 9076, 1);
    PULSES = array<int16_t>(d, 9078, 5);
    RADIX = array<int16_t>(d, 9088, 15);
    RADIX_INVERSE = array<int16_t>(d, 9118, 15);
    POSITIONS = array<int16_t>(d, 9148, 600);
    SHAPE_SELECT = array<int16_t>(d, 10348, 15);
    SHAPES = array<int16_t>(d, 10378, 240);
    ALT_A = array<int16_t>(d, 10858, 640);
    ALT_B = array<int16_t>(d, 12138, 640);
    INITIAL_GAIN = read_le<float>(d, 13418);
    Q__CURRENT_CDF_BASIS = array<int8_t>(d, 13422, 2240);
    Q__CURRENT_CDF_WEIGHT = array<int8_t>(d, 15662, 3520);
    Q__CURRENT_WEIGHT = array<int8_t>(d, 19182, 10120);
    Q__NET_W1 = array<int8_t>(d, 29302, 3520);
    Q__NET_W2 = array<int8_t>(d, 32822, 128);
    Q__NET_W3 = array<int8_t>(d, 32950, 8);
    Q__PREVIOUS_CDF_BASIS = array<int8_t>(d, 32958, 2240);
    Q__PREVIOUS_CDF_WEIGHT = array<int8_t>(d, 35198, 3520);
    Q__PREVIOUS_WEIGHT = array<int8_t>(d, 38718, 440);
    Q__TRAILING_CDF_BASIS = array<int8_t>(d, 39158, 2240);
    Q__TRAILING_CDF_WEIGHT = array<int8_t>(d, 41398, 3520);
    Q__TRAILING_WEIGHT = array<int8_t>(d, 44918, 440);
    S__CURRENT_CDF_BASIS = floats(d, 45358, 140);
    S__CURRENT_CDF_WEIGHT = floats(d, 45918, 16);
    S__CURRENT_WEIGHT = floats(d, 45982, 46);
    S__NET_W1 = floats(d, 46166, 16);
    S__NET_W2 = floats(d, 46230, 8);
    S__NET_W3 = floats(d, 46262, 1);
    S__PREVIOUS_CDF_BASIS = floats(d, 46266, 140);
    S__PREVIOUS_CDF_WEIGHT = floats(d, 46826, 16);
    S__PREVIOUS_WEIGHT = floats(d, 46890, 2);
    S__TRAILING_CDF_BASIS = floats(d, 46898, 140);
    S__TRAILING_CDF_WEIGHT = floats(d, 47458, 16);
    S__TRAILING_WEIGHT = floats(d, 47522, 2);
    V__CURRENT_BIAS = floats(d, 47530, 46);
    V__CURRENT_CDF_BIAS = floats(d, 47714, 16);
    V__CURRENT_CDF_MEAN = floats(d, 47778, 140);
    V__CURRENT_VOICE_RATE = floats(d, 48338, 1);
    V__MEAN = floats(d, 48342, 220);
    V__NET_B1 = floats(d, 49222, 16);
    V__NET_B2 = floats(d, 49286, 8);
    V__NET_B3 = floats(d, 49318, 1);
    V__PREVIOUS_BIAS = floats(d, 49322, 2);
    V__PREVIOUS_CDF_BIAS = floats(d, 49330, 16);
    V__PREVIOUS_CDF_MEAN = floats(d, 49394, 140);
    V__PREVIOUS_VOICE_RATE = floats(d, 49954, 1);
    V__SCALE = floats(d, 49958, 220);
    V__TRAILING_BIAS = floats(d, 50838, 2);
    V__TRAILING_CDF_BIAS = floats(d, 50846, 16);
    V__TRAILING_CDF_MEAN = floats(d, 50910, 140);
    V__TRAILING_VOICE_RATE = floats(d, 51470, 1);
    LOG_LAGS = floats(d, 51474, 141);
    EDGES = floats(d, 52038, 140);
    F32_TWIDDLE_REAL = floats(d, 52598, 1024);
    F32_TWIDDLE_IMAG = floats(d, 56694, 1024);
    F32_HANN = floats(d, 60790, 160);
    F32_BAND_WEIGHTS = floats(d, 61430, 486);
    F32_BAND_START = indices(d, 63374, 20);
    F32_BAND_OFFSET = indices(d, 63454, 21);
    F32_HANN_ENERGY = read_le<float>(d, 63538);
}

const PlannerModel& planner_model() {
    static const PlannerModel m;
    return m;
}

// ---- celp_parameters.bin ----
namespace celp_config {
namespace {
const Blob& data() {
    static const Blob b = require("celp_parameters");
    return b;
}
float scalar(std::size_t offset) { return read_le<float>(data(), offset); }
} // namespace
float pitch_gain(std::size_t i) { return scalar(4 * i); }
float log_gain(std::size_t i) { return scalar(512 + 4 * i); }
float gain_bias() { return scalar(1024); }
float low(std::size_t i, std::size_t k) { return scalar(1028 + 4 * (5 * i + k)); }
float high(std::size_t i, std::size_t k) { return scalar(2308 + 4 * (5 * i + k)); }
float ma(std::size_t mode, std::size_t tap, std::size_t k) { return scalar(3588 + 4 * (mode * 40 + tap * 10 + k)); }
} // namespace celp_config

// ---- synthesis_models.rs ----
namespace {
struct HmlArray {
    std::vector<std::size_t> shape;
    std::vector<float> data;
};
std::map<std::string, HmlArray> hml_arrays(const Blob& b) {
    std::size_t at = 0;
    auto byte = [&]() -> std::size_t {
        if (at >= b.size) throw std::runtime_error("jr_voice: truncated HML1");
        return b.data[at++];
    };
    auto integer = [&]() -> std::size_t {
        const auto v = read_le<uint32_t>(b, at);
        at += 4;
        return v;
    };
    if (b.size < 4 || std::memcmp(b.data, "HML1", 4) != 0) throw std::runtime_error("jr_voice: not an HML1 model");
    at = 4;
    const std::size_t count = integer();
    std::map<std::string, HmlArray> out;
    for (std::size_t n = 0; n < count; ++n) {
        const std::size_t length = byte();
        if (at + length > b.size) throw std::runtime_error("jr_voice: truncated HML1");
        std::string name(reinterpret_cast<const char*>(b.data + at), length);
        at += length;
        const std::size_t rank = byte();
        HmlArray a;
        std::size_t total = 1;
        for (std::size_t r = 0; r < rank; ++r) {
            a.shape.push_back(integer());
            total *= a.shape.back();
        }
        a.data.resize(total);
        for (std::size_t i = 0; i < total; ++i) {
            a.data[i] = read_le<float>(b, at);
            at += 4;
            if (!std::isfinite(a.data[i])) throw std::runtime_error("jr_voice: non-finite model value");
        }
        if (!out.emplace(std::move(name), std::move(a)).second) throw std::runtime_error("jr_voice: duplicate HML1 array");
    }
    if (at != b.size) throw std::runtime_error("jr_voice: trailing HML1 bytes");
    return out;
}
HmlArray take(std::map<std::string, HmlArray>& a, const std::string& name) {
    auto it = a.find(name);
    if (it == a.end()) throw std::runtime_error("jr_voice: missing HML1 array " + name);
    HmlArray v = std::move(it->second);
    a.erase(it);
    return v;
}
} // namespace

namespace {
using V8 = float __attribute__((vector_size(32)));

// 出力 o .. o+7 の内積を 8 レーンで同時に取る。各レーンの和は原本と同じく i の順に 1 本の累算器で取る(FMA にしない)ので、
// 1 出力ずつ計算したときと同じ値になる
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
__attribute__((target_clones("avx2", "default")))
#endif
void dot8(const float* x, std::size_t input, const float* columns, std::size_t output, std::size_t o, float* sums) {
    V8 acc{};
    for (std::size_t i = 0; i < input; ++i) {
        V8 xi, w;
        for (std::size_t l = 0; l < 8; ++l) xi[l] = x[i];
        std::memcpy(&w, columns + i * output + o, sizeof w);
        acc += xi * w;
    }
    std::memcpy(sums, &acc, sizeof acc);
}
} // namespace

std::vector<float> Layer::apply(std::span<const float> x, bool activation) const {
    if (x.size() != input) throw std::runtime_error("jr_voice: layer input size");
    std::vector<float> out(output);
    std::size_t first = 0;
    if (columns.size() == weight.size())
        for (; first + 8 <= output; first += 8) {
            float sums[8];
            dot8(x.data(), input, columns.data(), output, first, sums);
            for (std::size_t l = 0; l < 8; ++l) {
                const float sum = sums[l] + bias[first + l];
                out[first + l] = activation ? std::tanh(sum) : sum;
            }
        }
    for (std::size_t o = first; o < output; ++o) {
        float sum = 0.f;
        for (std::size_t i = 0; i < input; ++i) sum += x[i] * weight[o * input + i];
        sum = sum + bias[o];
        out[o] = activation ? std::tanh(sum) : sum;
    }
    return out;
}

Network::Network(Blob bytes) {
    auto a = hml_arrays(bytes);
    mean_ = take(a, "mean").data;
    scale_ = take(a, "scale").data;
    if (mean_.size() != scale_.size()) throw std::runtime_error("jr_voice: mean/scale size");
    for (float s : scale_)
        if (!(s > 0.f)) throw std::runtime_error("jr_voice: non-positive scale");
    for (;;) {
        auto it = a.find("w" + std::to_string(layers_.size()));
        if (it == a.end()) break;
        HmlArray w = std::move(it->second);
        a.erase(it);
        if (w.shape.size() != 2) throw std::runtime_error("jr_voice: weight rank");
        auto bias = take(a, "b" + std::to_string(layers_.size())).data;
        if (bias.size() != w.shape[0]) throw std::runtime_error("jr_voice: bias size");
        Layer layer{w.shape[1], w.shape[0], std::move(w.data), std::move(bias), {}};
        layer.columns.resize(layer.weight.size());
        for (std::size_t o = 0; o < layer.output; ++o)
            for (std::size_t i = 0; i < layer.input; ++i) layer.columns[i * layer.output + o] = layer.weight[o * layer.input + i];
        layers_.push_back(std::move(layer));
    }
    if (auto it = a.find("output_scale"); it != a.end()) {
        output_scale_ = std::move(it->second.data);
        a.erase(it);
    } else {
        output_scale_.assign(layers_.back().output, 1.f);
    }
    if (!a.empty()) throw std::runtime_error("jr_voice: unexpected HML1 arrays");
    if (layers_.at(0).input != mean_.size()) throw std::runtime_error("jr_voice: input size");
}

std::vector<float> Network::normalize(std::span<const float> x) const {
    if (x.size() != mean_.size()) throw std::runtime_error("jr_voice: normalize size");
    std::vector<float> out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) out[i] = rs::clamp((x[i] - mean_[i]) / scale_[i], -6.f, 6.f);
    return out;
}

std::vector<float> Network::forward(std::span<const float> x) const {
    auto value = normalize(x);
    for (std::size_t i = 0; i < layers_.size(); ++i) value = layers_[i].apply(value, i + 1 < layers_.size());
    const std::size_t n = std::min(value.size(), output_scale_.size());
    for (std::size_t i = 0; i < n; ++i) value[i] *= output_scale_[i];
    return value;
}

std::array<float, 128> Network::temporal_embedding(const std::array<float, 707>& x) const {
    const auto v = layers_[0].apply(normalize(x), true);
    if (v.size() != 128) throw std::runtime_error("jr_voice: embedding size");
    std::array<float, 128> out;
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

std::array<float, 34> Network::temporal_finish(const std::array<std::array<float, 128>, 7>& context) const {
    std::vector<float> value;
    value.reserve(7 * 128);
    for (const auto& c : context) value.insert(value.end(), c.begin(), c.end());
    for (std::size_t i = 1; i < layers_.size(); ++i) value = layers_[i].apply(value, i + 1 < layers_.size());
    if (value.size() != 34) throw std::runtime_error("jr_voice: temporal output size");
    std::array<float, 34> out;
    std::copy(value.begin(), value.end(), out.begin());
    return out;
}

GainModel::GainModel(Blob bytes) {
    auto a = hml_arrays(bytes);
    mean_ = take(a, "mean").data;
    scale_ = take(a, "scale").data;
    auto w = take(a, "weight");
    if (mean_.size() != 137 || scale_.size() != 137 || w.shape != std::vector<std::size_t>{138, 2}) throw std::runtime_error("jr_voice: gain model shape");
    for (float s : scale_)
        if (!(s > 0.f)) throw std::runtime_error("jr_voice: non-positive scale");
    if (!a.empty()) throw std::runtime_error("jr_voice: unexpected HML1 arrays");
    weight_ = std::move(w.data);
}

std::array<float, 2> GainModel::predict(const float* x) const {
    std::array<float, 137> z;
    for (std::size_t i = 0; i < 137; ++i) z[i] = rs::clamp((x[i] - mean_[i]) / scale_[i], -6.f, 6.f);
    std::array<float, 2> out;
    for (std::size_t o = 0; o < 2; ++o) {
        float sum = 0.f;
        for (std::size_t i = 0; i < 137; ++i) sum += z[i] * weight_[2 * (i + 1) + o];
        out[o] = sum + weight_[o];
    }
    return out;
}

const SynthesisModels& synthesis_models() {
    static const SynthesisModels m{Network(require("source")), Network(require("spectrum")), GainModel(require("gain")), Network(require("temporal"))};
    return m;
}

} // namespace spear::jr_train::voice
