// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/receiver-core/src/pipeline.rs
#include "pipeline.hpp"

#include "protocol/voice_frame.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

namespace spear::jr_train {

std::string now_rfc3339() {
    const auto now = std::chrono::system_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    const std::time_t secs = static_cast<std::time_t>(ns / 1'000'000'000);
    const long frac = static_cast<long>(ns % 1'000'000'000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    char out[96];
    std::snprintf(out, sizeof out, "%s.%09ld+00:00", buf, frac);
    return out;
}

ChannelConfig channel_config(const ChannelDef& c) {
    return {std::to_string(static_cast<uint64_t>(c.frequency_hz)), c.label, c.frequency_hz, c.service == Service::Common ? protocol::Format::Common : protocol::Format::Slotted};
}

Pipeline::Pipeline(ChannelConfig cfg, double rate, double center)
    : config(std::move(cfg)), rate_(rate), center_(center), dsp_(rate, config.frequency_hz - center), data_(config.format) {
    if (!std::isfinite(center) || !std::isfinite(config.frequency_hz) || config.frequency_hz <= 0) throw std::invalid_argument("Invalid channel frequency");
}

void Pipeline::discontinuity() {
    dsp_ = dsp::Demodulator(rate_, config.frequency_hz - center_);
    sync_ = protocol::Synchronizer{};
    voice_ = voice::Voice{};
    data_.reset_assembly();
    origin_.reset();
    last_offset_.reset();
    last_speech_.reset();
    segment_samples_ = 0;
    history_.clear();
    events_.clear();
    ++status_.epoch;
    ++status_.discontinuities;
    status_.frame_locked = false;
    status_.carrier_locked = false;
    status_.constellation.clear();
    status_.messages.clear();
}

std::vector<Audio> Pipeline::process(std::span<const std::complex<float>> iq, const std::function<void(const FrameEvent&)>& observe) {
    status_.iq_samples += iq.size();
    segment_samples_ += iq.size();
    dibits_.clear();
    dsp_.process(iq, dibits_);
    const auto snap = dsp_.snapshot();
    std::vector<protocol::Frame> frames;
    for (uint8_t d : dibits_) sync_.push(d, frames);
    status_.carrier_locked = snap.carrier_locked != 0;
    status_.frequency_error_hz = snap.frequency_offset_hz;
    status_.frame_locked = sync_.locked;
    if (!iq.empty()) {
        double power = 0.0;
        for (const auto& s : iq) power += static_cast<double>(s.real()) * static_cast<double>(s.real()) + static_cast<double>(s.imag()) * static_cast<double>(s.imag());
        power /= static_cast<double>(iq.size());
        status_.input_dbfs = 10.0 * std::log10(std::max(power, 1e-12));
    }
    std::vector<Audio> audio;
    for (const auto& f : frames) {
        auto a = frame(f, observe);
        audio.insert(audio.end(), std::make_move_iterator(a.begin()), std::make_move_iterator(a.end()));
    }
    // RF の搬送波が消えても合成の先読みを吐き出す
    auto tail = flush_idle(static_cast<uint64_t>(static_cast<double>(segment_samples_) / rate_ * 9600.0));
    audio.insert(audio.end(), std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()));
    return audio;
}

std::vector<Audio> Pipeline::frame(const protocol::Frame& f, const std::function<void(const FrameEvent&)>& observe) {
    const bool new_stream = (origin_ && (f.bit_offset < *origin_ || (f.bit_offset - *origin_) % protocol::kFrameBits != 0 ||
                                         (f.bit_offset - *origin_) / protocol::kFrameBits > UINT32_MAX)) ||
                            (last_offset_ && f.bit_offset <= *last_offset_);
    if (new_stream) {
        voice_ = voice::Voice{};
        data_.reset_assembly();
        origin_.reset();
        last_speech_.reset();
        ++status_.epoch;
    }
    if (!origin_) origin_ = f.bit_offset;
    const auto index = static_cast<uint32_t>((f.bit_offset - *origin_) / protocol::kFrameBits);
    last_offset_ = f.bit_offset;
    ++status_.frames;
    if (f.sync_matched) ++status_.synchronized_frames;
    const auto speech = protocol::decode_voice(f.received);
    std::optional<uint16_t> corrected;
    if (speech) {
        corrected = speech->corrected_bits;
        last_speech_ = f.bit_offset;
        ++status_.voice_frames;
        status_.corrected_bits += speech->corrected_bits;
        voice_.push(index, speech->payload);
    } else {
        ++status_.rejected_voice;
        voice_.push(index, std::nullopt);
    }
    auto messages = data_.decode(f.received, f.bit_offset);
    for (const auto& m : messages) {
        if (m.operation_summary) protocol::OperationReception::update(status_.operation_monitor, m, now_rfc3339());
        if (events_.size() == 128) events_.pop_front();
        events_.push_back(m);
        auto existing = std::find_if(history_.begin(), history_.end(), [&](const Received& r) {
            return r.message.kind == m.kind && r.message.identity == m.identity &&
                   (m.kind == "ChannelActivity" || (r.message.raw_hex == m.raw_hex && r.message.timestamp == m.timestamp));
        });
        if (existing != history_.end()) {
            existing->message = m;
            ++existing->receptions;
        } else {
            if (history_.size() == 128) history_.pop_front();
            history_.push_back({m, 1});
        }
    }
    if (observe) {
        FrameEvent ev;
        ev.epoch = status_.epoch;
        ev.index = index;
        ev.frame = f;
        ev.corrected_bits = corrected;
        ev.messages = std::move(messages);
        observe(ev);
    }
    auto audio = drain();
    auto tail = flush_idle(f.bit_offset);
    audio.insert(audio.end(), std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()));
    return audio;
}

// 音声が 100 ms 無ければ合成の先読みを吐き出す(呼の終わりの推定ではない: 監視側は 1 s の欠落まで許す)
std::vector<Audio> Pipeline::flush_idle(uint64_t bit_offset) {
    if (last_speech_ && (bit_offset > *last_speech_ ? bit_offset - *last_speech_ : 0) >= 960) {
        voice_.flush();
        auto audio = drain();
        voice_ = voice::Voice{};
        last_speech_.reset();
        return audio;
    }
    return {};
}

std::vector<Audio> Pipeline::drain() {
    std::vector<Audio> audio;
    while (auto out = voice_.read()) {
        ++status_.audio_frames;
        audio.push_back(Audio{config.id, status_.epoch, out->index, out->pcm});
    }
    return audio;
}

std::vector<Audio> Pipeline::flush() {
    voice_.flush();
    return drain();
}

std::vector<protocol::Message> Pipeline::take_messages() {
    std::vector<protocol::Message> out(std::make_move_iterator(events_.begin()), std::make_move_iterator(events_.end()));
    events_.clear();
    return out;
}

const PipelineStatus& Pipeline::snapshot() {
    status_.evm_percent = dsp_.read_signal(status_.constellation, 256);
    status_.data = data_.stats;
    status_.messages.assign(history_.rbegin(), history_.rend());
    return status_;
}

} // namespace spear::jr_train
