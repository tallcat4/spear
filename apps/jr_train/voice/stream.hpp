// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// index 付きの受信: 合計 7 フレームの先読みと、欠落の有界な排出。原本: src/stream.rs。
#pragma once

#include "correction.hpp"
#include "synthesis.hpp"
#include "voice.hpp"

#include <array>
#include <optional>

namespace spear::jr_train::voice {

class VoiceStream {
public:
    bool has_output() const { return output_count_ != 0; }
    std::optional<Output> read();
    void push(uint32_t index, const PayloadBytes& payload);   // 失敗は Failure
    void missing_frame();
    void advance();
    void flush();

    uint32_t synthesized = 0;
    uint32_t emitted = 0;

private:
    struct QueuedFrame {
        PayloadBytes payload{};
        uint32_t index = 0;
    };
    bool can_produce(uint32_t before) const { return synthesized == before || !pipeline_.has_pending(); }
    void render(const Prediction& p, bool draining_gap);
    void emit(const Output& frame);
    void emit_synth(const SynthOutput& out);

    Predictor predictor_;
    Correction correction_;
    Numeric numeric_;
    Pipeline pipeline_;
    uint64_t next_index_ = 0;
    bool fade_in_ = false, known_missing_ = false;
    std::array<QueuedFrame, kLookahead + 5> queue_{};
    std::size_t head_ = 0, count_ = 0;
    std::optional<uint32_t> last_fed_;
    std::array<Output, 16> output_{};
    std::size_t output_head_ = 0, output_count_ = 0;
};

} // namespace spear::jr_train::voice
