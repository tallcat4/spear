// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 1 チャネルの受信処理。原本: crates/receiver-core/src/pipeline.rs(commit dfc7440)。
//   IQ → 受信 DSP → フレーム同期 → [フレームごと] 音声フレームの誤り訂正 → 音声合成(先読み 7 フレーム)
//                                               データフレーム → パケット → 電文
//   連続区間(epoch): フレームの bit 位置が 384 の倍数で進まなくなったら新しい区間にする(音声・パケットの組立をやり直す)。
//   音声が 960 bit(100 ms)途切れたら合成器の先読みを吐き出す(呼の終わりの推定ではない。それは CallMonitor が 1 s で行う)。
// Qt / UHD / spear_core に依存しない。
#pragma once

#include "dsp/jr_dsp.hpp"
#include "presets.hpp"
#include "protocol/data.hpp"
#include "protocol/frame_sync.hpp"
#include "voice/voice.hpp"

#include <complex>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace spear::jr_train {

struct ChannelConfig {
    std::string id;           // 原本と同じく周波数の整数表記("352537500")
    std::string label;
    double frequency_hz = 0;
    protocol::Format format = protocol::Format::Slotted;
};

// プリセットのチャネル → 受信設定(原本 presets.rs: id = 周波数の整数表記、共用波だけ Common)
ChannelConfig channel_config(const ChannelDef& c);

struct Audio {
    std::string channel;
    uint64_t epoch = 0;
    uint32_t index = 0;       // epoch 内の 40 ms フレーム番号
    voice::Pcm pcm{};
};

struct Received {
    protocol::Message message;
    uint64_t receptions = 0;  // 同じ電文の受信回数(チャネル状況は種別と識別子が同じなら同じとみなす)
};

struct PipelineStatus {
    uint64_t iq_samples = 0, frames = 0, synchronized_frames = 0, voice_frames = 0, rejected_voice = 0, corrected_bits = 0, audio_frames = 0, discontinuities = 0;
    bool frame_locked = false, carrier_locked = false;
    double frequency_error_hz = 0, evm_percent = 0, input_dbfs = -120;
    protocol::DataStats data;
    std::vector<std::complex<float>> constellation;
    std::vector<Received> messages;          // 新しい順(snapshot の時点)
    std::optional<protocol::OperationReception> operation_monitor;
    uint64_t epoch = 0;
};

struct FrameEvent {
    uint64_t epoch = 0;
    uint32_t index = 0;
    protocol::Frame frame;
    std::optional<uint16_t> corrected_bits;  // 音声フレームとして受理したときだけ
    std::vector<protocol::Message> messages;
};

// 現在の UTC を RFC 3339(原本 chrono の to_rfc3339 と同じ形: "2026-10-15T08:00:00.123456789+00:00")
std::string now_rfc3339();

class Pipeline {
public:
    Pipeline(ChannelConfig config, double rate, double center);
    ChannelConfig config;

    void discontinuity();
    // IQ を処理し、完成した音声を返す。フレームごとに observe を呼ぶ。原本の Err は std::runtime_error
    std::vector<Audio> process(std::span<const std::complex<float>> iq, const std::function<void(const FrameEvent&)>& observe = {});
    std::vector<Audio> frame(const protocol::Frame& f, const std::function<void(const FrameEvent&)>& observe = {});
    std::vector<Audio> flush();
    std::vector<protocol::Message> take_messages();
    const PipelineStatus& snapshot();       // コンスタレーションと EVM、電文の履歴を更新して返す
    const PipelineStatus& status() const { return status_; }

private:
    std::vector<Audio> flush_idle(uint64_t bit_offset);
    std::vector<Audio> drain();
    double rate_, center_;
    dsp::Demodulator dsp_;
    protocol::Synchronizer sync_;
    voice::Voice voice_;
    protocol::DataDecoder data_;
    std::vector<uint8_t> dibits_;
    std::optional<uint64_t> origin_, last_offset_, last_speech_;
    uint64_t segment_samples_ = 0;
    std::deque<Received> history_;
    std::deque<protocol::Message> events_;
    PipelineStatus status_;
};

} // namespace spear::jr_train
