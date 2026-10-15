// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 学習済み係数(models/*.bin、バイナリに埋め込み)の型付きの見え方。原本: src/models.rs、src/model_bindings/*.rs、
// src/synthesis_models.rs、src/celp/config.rs。オフセットと形は原本の定義のまま。値はすべてリトルエンディアン。
// 全インスタンスで共有し、初回利用時に一度だけ展開する(関数内 static、スレッド安全)。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spear::jr_train::voice {

// 埋め込みバイト列(CMake が models/*.bin から生成する voice_models_data.cpp)
struct Blob {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
};
Blob embedded_model(const char* name);   // "predictor" など(拡張子なし)。無ければ data = nullptr

// ---- 原本 models.rs ----
struct Tensor {
    const float* data = nullptr;
    std::size_t rows = 0, cols = 0;
    const int8_t* quantized = nullptr;
    std::size_t quantized_len = 0;
    float quantum = 1.f;
    const uint8_t* sparse_rows = nullptr;
    const uint16_t* sparse_offsets = nullptr;   // cols + 1 個(疎でなければ nullptr)

    bool sparse() const { return sparse_offsets != nullptr; }
    bool is_quantized() const { return quantized_len != 0; }
    float get(std::size_t i) const {
        if (sparse_offsets) {
            const std::size_t col = i % cols, row = i / cols;
            for (std::size_t p = sparse_offsets[col]; p < sparse_offsets[col + 1]; ++p)
                if (sparse_rows[p] == row) return static_cast<float>(quantized[p]) * quantum;
            return 0.f;
        }
        if (quantized_len) return static_cast<float>(quantized[i]) * quantum;
        return data[i];
    }
};

struct Mlp {
    Tensor x_mean, x_scale, y_mean, y_scale;
    std::array<Tensor, 3> weight, bias;
};

struct Int8Matrix {
    const int8_t* data = nullptr;
    const float* scale = nullptr;
    std::size_t columns = 0;
    // 原本 Int8Matrix::dense
    void dense(std::span<const float> input, const float* bias, float* output) const {
        for (std::size_t j = 0; j < columns; ++j) {
            float sum = 0.f;
            for (std::size_t i = 0; i < input.size(); ++i) sum += input[i] * static_cast<float>(data[i * columns + j]);
            output[j] = sum * scale[j] + bias[j];
        }
    }
};

// ---- model_bindings/predictor.rs ----
struct PredictorModel {
    std::vector<float> baseline_bias, baseline_weight, calibration_bias, calibration_mean, calibration_scale, calibration_weight;
    std::vector<float> context_layer_0_bias, context_layer_1_bias, context_layer_1_weight, context_layer_2_bias, context_layer_2_weight;
    std::vector<int8_t> context_layer_0_weight_q;
    std::vector<float> context_x_mean, context_x_scale, context_y_mean, context_y_scale;
    std::vector<float> levels_layer_0_bias, levels_layer_0_weight, levels_layer_1_bias, levels_layer_1_weight, levels_layer_2_bias, levels_layer_2_weight;
    std::vector<float> levels_x_mean, levels_x_scale, levels_y_mean, levels_y_scale;
    std::vector<float> profile_weight, recurrent_gate_bias;
    std::vector<int8_t> recurrent_input_weight_q;
    std::vector<uint8_t> recurrent_input_weight_rows;
    std::vector<uint16_t> recurrent_input_weight_offsets;
    std::vector<float> recurrent_output_bias;
    std::vector<int8_t> recurrent_output_weight_q, recurrent_recurrent_weight_q;
    std::vector<uint8_t> recurrent_recurrent_weight_rows;
    std::vector<uint16_t> recurrent_recurrent_weight_offsets;
    std::vector<float> recurrent_x_mean, recurrent_x_scale, recurrent_y_mean, recurrent_y_scale;
    std::vector<float> refinement_pitch_bias, refinement_pitch_mean, refinement_pitch_scale, refinement_pitch_weight;
    std::vector<float> refinement_spectral_bias, refinement_spectral_mean, refinement_spectral_scale, refinement_spectral_table, refinement_spectral_weight;
    std::vector<float> spectrum_layer_0_bias, spectrum_layer_1_bias, spectrum_layer_1_weight, spectrum_layer_2_bias, spectrum_layer_2_weight;
    std::vector<int8_t> spectrum_layer_0_weight_q;
    std::vector<float> spectrum_x_mean, spectrum_x_scale, spectrum_y_mean, spectrum_y_scale;
    std::vector<float> voicing_bias, voicing_weight, voicing_x_mean, voicing_x_scale;
    std::vector<float> context_blend, level_blend, spectral_blend, recurrent_blend;

    Tensor BASELINE_BIAS, BASELINE_WEIGHT, CALIBRATION_BIAS, CALIBRATION_MEAN, CALIBRATION_SCALE, CALIBRATION_WEIGHT;
    Tensor PROFILE_WEIGHT, RECURRENT_GATE_BIAS, RECURRENT_INPUT_WEIGHT, RECURRENT_OUTPUT_BIAS, RECURRENT_OUTPUT_WEIGHT, RECURRENT_RECURRENT_WEIGHT;
    Tensor RECURRENT_X_MEAN, RECURRENT_X_SCALE, RECURRENT_Y_MEAN, RECURRENT_Y_SCALE;
    Tensor REFINEMENT_PITCH_BIAS, REFINEMENT_PITCH_MEAN, REFINEMENT_PITCH_SCALE, REFINEMENT_PITCH_WEIGHT;
    Tensor REFINEMENT_SPECTRAL_BIAS, REFINEMENT_SPECTRAL_MEAN, REFINEMENT_SPECTRAL_SCALE, REFINEMENT_SPECTRAL_TABLE, REFINEMENT_SPECTRAL_WEIGHT;
    Tensor VOICING_BIAS, VOICING_WEIGHT, VOICING_X_MEAN, VOICING_X_SCALE;
    Mlp CONTEXT, LEVELS, SPECTRUM;
    float PITCH_BLEND = 0.f;
    std::array<std::array<std::size_t, 8>, 7> PROFILE_GROUPS{};
    std::array<std::size_t, 7> PROFILE_LENGTHS{}, PROFILE_OFFSETS{};

    PredictorModel();
    PredictorModel(const PredictorModel&) = delete;
    PredictorModel& operator=(const PredictorModel&) = delete;
};
const PredictorModel& predictor_model();

// ---- model_bindings/correction.rs ----
struct CorrectionModel {
    std::vector<float> GAIN_MEAN, GAIN_SCALE, GAIN_WEIGHT, LINEAR_MEAN, LINEAR_SCALE;
    std::vector<int8_t> linear_coefficient_q;
    std::vector<float> linear_coefficient_scale;
    std::vector<int16_t> MAIN_TABLE;
    std::vector<float> STUDENT_X_MEAN, STUDENT_X_SCALE, STUDENT_Y_MEAN, STUDENT_Y_SCALE;
    std::vector<int8_t> student_layer_0_weight_q, student_layer_1_weight_q, student_layer_2_weight_q;
    std::vector<float> student_layer_0_weight_scale, student_layer_1_weight_scale, student_layer_2_weight_scale;
    std::vector<float> STUDENT_LAYER_0_BIAS, STUDENT_LAYER_1_BIAS, STUDENT_LAYER_2_BIAS;
    Int8Matrix LINEAR_COEFFICIENT, STUDENT_LAYER_0_WEIGHT, STUDENT_LAYER_1_WEIGHT, STUDENT_LAYER_2_WEIGHT;
    // 原本 Int8Matrix::get
    float linear_coefficient(std::size_t i) const { return static_cast<float>(linear_coefficient_q[i]) * linear_coefficient_scale[i % 16]; }

    CorrectionModel();
    CorrectionModel(const CorrectionModel&) = delete;
    CorrectionModel& operator=(const CorrectionModel&) = delete;
};
const CorrectionModel& correction_model();

// ---- model_bindings/planner.rs ----
struct PlannerModel {
    // input_parameters
    std::vector<float> LSF_MEAN, SPECTRAL, MA, GAIN_LOG, GAIN_ADAPTIVE;
    std::vector<uint16_t> CLASS_BASE, INDEX_LIMIT;
    std::vector<int16_t> PULSES, RADIX, RADIX_INVERSE, POSITIONS, SHAPE_SELECT, SHAPES, ALT_A, ALT_B;
    float INITIAL_GAIN = 0.f;
    // learned_parameters(Q__ は int8、S__ は列ごとの尺度、V__ は float)
    std::vector<int8_t> Q__CURRENT_CDF_BASIS, Q__CURRENT_CDF_WEIGHT, Q__CURRENT_WEIGHT, Q__NET_W1, Q__NET_W2, Q__NET_W3;
    std::vector<int8_t> Q__PREVIOUS_CDF_BASIS, Q__PREVIOUS_CDF_WEIGHT, Q__PREVIOUS_WEIGHT, Q__TRAILING_CDF_BASIS, Q__TRAILING_CDF_WEIGHT, Q__TRAILING_WEIGHT;
    std::vector<float> S__CURRENT_CDF_BASIS, S__CURRENT_CDF_WEIGHT, S__CURRENT_WEIGHT, S__NET_W1, S__NET_W2, S__NET_W3;
    std::vector<float> S__PREVIOUS_CDF_BASIS, S__PREVIOUS_CDF_WEIGHT, S__PREVIOUS_WEIGHT, S__TRAILING_CDF_BASIS, S__TRAILING_CDF_WEIGHT, S__TRAILING_WEIGHT;
    std::vector<float> V__CURRENT_BIAS, V__CURRENT_CDF_BIAS, V__CURRENT_CDF_MEAN, V__CURRENT_VOICE_RATE, V__MEAN, V__NET_B1, V__NET_B2, V__NET_B3;
    std::vector<float> V__PREVIOUS_BIAS, V__PREVIOUS_CDF_BIAS, V__PREVIOUS_CDF_MEAN, V__PREVIOUS_VOICE_RATE, V__SCALE;
    std::vector<float> V__TRAILING_BIAS, V__TRAILING_CDF_BIAS, V__TRAILING_CDF_MEAN, V__TRAILING_VOICE_RATE, LOG_LAGS, EDGES;
    // measurement_parameters
    std::vector<float> F32_TWIDDLE_REAL, F32_TWIDDLE_IMAG, F32_HANN, F32_BAND_WEIGHTS;
    std::vector<std::size_t> F32_BAND_START, F32_BAND_OFFSET;
    float F32_HANN_ENERGY = 0.f;

    PlannerModel();
    PlannerModel(const PlannerModel&) = delete;
    PlannerModel& operator=(const PlannerModel&) = delete;
};
const PlannerModel& planner_model();

// ---- celp/config.rs: celp_parameters.bin ----
namespace celp_config {
float pitch_gain(std::size_t i);
float log_gain(std::size_t i);
float gain_bias();
float low(std::size_t i, std::size_t k);
float high(std::size_t i, std::size_t k);
float ma(std::size_t mode, std::size_t tap, std::size_t k);
} // namespace celp_config

// ---- synthesis_models.rs(HML1 形式の source / spectrum / gain / temporal)----
struct Layer {
    std::size_t input = 0, output = 0;
    std::vector<float> weight, bias;
    std::vector<float> apply(std::span<const float> x, bool activation) const;
};
class Network {
public:
    explicit Network(Blob bytes);
    std::vector<float> forward(std::span<const float> x) const;
    std::array<float, 128> temporal_embedding(const std::array<float, 707>& x) const;
    std::array<float, 34> temporal_finish(const std::array<std::array<float, 128>, 7>& context) const;

private:
    std::vector<float> normalize(std::span<const float> x) const;
    std::vector<float> mean_, scale_;
    std::vector<Layer> layers_;
    std::vector<float> output_scale_;
};
class GainModel {
public:
    explicit GainModel(Blob bytes);
    std::array<float, 2> predict(const float* x /*137*/) const;

private:
    std::vector<float> mean_, scale_, weight_;
};
struct SynthesisModels {
    Network source, spectrum;
    GainModel gain;
    Network temporal;
};
const SynthesisModels& synthesis_models();

} // namespace spear::jr_train::voice
