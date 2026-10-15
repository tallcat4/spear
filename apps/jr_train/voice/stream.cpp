// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/stream.rs
#include "stream.hpp"

#include "common.hpp"

#include <limits>

namespace spear::jr_train::voice {

std::optional<Output> VoiceStream::read() {
    if (output_count_ == 0) return std::nullopt;
    const Output r = output_[output_head_];
    output_head_ = (output_head_ + 1) % output_.size();
    --output_count_;
    return r;
}

void VoiceStream::push(uint32_t index, const PayloadBytes& payload) {
    if (count_ == queue_.size()) throw Failure{};
    known_missing_ = false;
    queue_[(head_ + count_) % queue_.size()] = QueuedFrame{payload, index};
    ++count_;
    advance();
}

void VoiceStream::missing_frame() {
    known_missing_ = true;
    advance();
}

void VoiceStream::advance() {
    const uint32_t before = synthesized;
    while (count_ != 0) {
        const QueuedFrame frame = queue_[head_];
        const bool gap = last_fed_ && static_cast<uint64_t>(frame.index) != static_cast<uint64_t>(*last_fed_) + 1;
        if (gap) {
            if (predictor_.pending() != 0) {
                if (!can_produce(before)) return;
                const auto p = predictor_.emit();
                render(p, true);
                continue;
            }
            if (pipeline_.has_pending()) {
                if (synthesized != before) return;
                if (auto out = pipeline_.finish()) emit_synth(*out);
                if (pipeline_.has_pending()) return;
            }
            predictor_.reset();
            correction_.reset();
            pipeline_ = Pipeline{};
            fade_in_ = true;
            last_fed_.reset();
        }
        const bool will_produce = predictor_.pending() == kLookahead;
        if (will_produce && !can_produce(before)) return;
        if (!last_fed_) next_index_ = frame.index;
        if (auto p = predictor_.decode(frame.payload)) render(*p, false);
        last_fed_ = frame.index;
        head_ = (head_ + 1) % queue_.size();
        --count_;
    }
    // 欠落スロットは不連続を示すが、最後の制御値は EOF か再開始で末尾のフェードが要るか決まるまで保つ
    if (known_missing_ && predictor_.pending() > 1 && can_produce(before)) {
        const auto p = predictor_.emit();
        render(p, false);
    }
    if (static_cast<uint32_t>(synthesized - before) > 1) throw Failure{};
}

void VoiceStream::flush() {
    while (count_ != 0) advance();
    while (predictor_.pending() != 0) {
        const auto p = predictor_.emit();
        render(p, false);
    }
    while (auto out = pipeline_.finish()) emit_synth(*out);
}

void VoiceStream::render(const Prediction& p, bool draining_gap) {
    const Controls corrected = correction_.process(p.bits, p.controls, numeric_);
    PayloadBytes payload{};
    for (std::size_t i = 0; i < 158; ++i) payload[i / 8] = static_cast<uint8_t>(payload[i / 8] | (p.bits[i] << (i % 8)));
    SynthInput input;
    input.key = next_index_;
    input.payload = payload;
    input.controls = corrected;
    input.fade_in = fade_in_;
    input.fade_out = draining_gap && predictor_.pending() == 0;
    if (auto out = pipeline_.push(input)) emit_synth(*out);
    fade_in_ = false;
    ++next_index_;
}

void VoiceStream::emit_synth(const SynthOutput& out) {
    if (out.index > std::numeric_limits<uint32_t>::max()) throw Failure{};
    Output o;
    o.index = static_cast<uint32_t>(out.index);
    o.pcm = out.pcm;
    emit(o);
}

void VoiceStream::emit(const Output& frame) {
    if (output_count_ == output_.size()) throw Failure{};
    output_[(output_head_ + output_count_) % output_.size()] = frame;
    ++output_count_;
    ++synthesized;
    ++emitted;
}

} // namespace spear::jr_train::voice
