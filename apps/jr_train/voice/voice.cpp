// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/receiver.rs(所有する安全なインターフェース)と src/lib.rs の Decoder(受理と退役の規則)。
// 原本で panic / Err(()) になる内部の失敗(Failure と標準例外)はここで捕まえ、デコーダを「失敗」にして VoiceError を投げる。
#include "voice.hpp"

#include "common.hpp"
#include "enhance.hpp"
#include "stream.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <variant>

namespace spear::jr_train::voice {

// 2 つの復号器は同じ面(push / missing_frame / read / flush / has_output / synthesized / emitted)を持つ
struct Voice::Impl {
    using Stream = std::variant<VoiceStream, EnhanceStream>;
    explicit Impl(Decoder d) : voice(d == Decoder::Enhance ? Stream(std::in_place_type<EnhanceStream>) : Stream(std::in_place_type<VoiceStream>)) {}
    Stream voice;
    std::optional<uint32_t> last_index;
    VoiceStatus status;

    // 原本 Decoder::guard。成功なら 0、失敗なら -4(failed を立てる)
    template <class F>
    int guard(F&& action, bool flush) {
        const auto start = std::chrono::steady_clock::now();
        bool ok = true;
        try {
            action();
        } catch (const Failure&) {
            ok = false;
        } catch (const std::exception&) {
            ok = false;
        }
        std::visit([&](auto& v) {
            status.synthesized = v.synthesized;
            status.output = v.emitted;
        }, voice);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        status.last_decode_us = static_cast<uint32_t>(std::min<long long>(us, std::numeric_limits<uint32_t>::max()));
        uint32_t& maximum = flush ? status.maximum_flush_us : status.maximum_frame_us;
        maximum = std::max(maximum, status.last_decode_us);
        if (ok) return 0;
        status.failed = 1;
        return -4;
    }
    // 原本 Decoder::ready
    int ready(uint32_t index) const {
        if (status.failed != 0 || status.finished != 0 || (last_index && index <= *last_index)) return -2;
        if (std::visit([](const auto& v) { return v.has_output(); }, voice)) return -3;
        return 0;
    }
};

Voice::Voice(Decoder decoder) : impl_(std::make_unique<Impl>(decoder)) {}
Voice::~Voice() = default;
Voice::Voice(Voice&&) noexcept = default;
Voice& Voice::operator=(Voice&&) noexcept = default;

void Voice::push(uint32_t index, const std::optional<Payload>& payload) {
    Impl& d = *impl_;
    if (d.ready(index) != 0) throw VoiceError("Voice decoder is not ready; drain output or restart");
    if (payload && ((*payload)[19] & 0xc0) != 0) throw VoiceError("Invalid CELP payload");
    const int code = d.guard(
        [&] {
            d.last_index = index;
            std::visit([&](auto& v) {
                if (payload) v.push(index, *payload);
                else v.missing_frame();
            }, d.voice);
        },
        false);
    if (code != 0) throw VoiceError("CELP synthesis failed; restart reception");
}

std::optional<Output> Voice::read() {
    return std::visit([](auto& v) { return v.read(); }, impl_->voice);
}

void Voice::flush() {
    Impl& d = *impl_;
    if (d.status.failed != 0) throw VoiceError("Voice decoder failed");
    if (d.status.finished != 0) return;
    const int code = d.guard(
        [&] {
            std::visit([](auto& v) { v.flush(); }, d.voice);
            d.status.finished = 1;
        },
        true);
    if (code != 0) throw VoiceError("CELP flush failed");
}

const VoiceStatus& Voice::status() const { return impl_->status; }

Decoder Voice::decoder() const { return std::holds_alternative<EnhanceStream>(impl_->voice) ? Decoder::Enhance : Decoder::JrVoice; }

} // namespace spear::jr_train::voice
