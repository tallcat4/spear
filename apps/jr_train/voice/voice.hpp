// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 音声合成(1 チャネル分)。原本: crates/jr-voice(jr_conventional_voice v1.3.0、commit dfc7440)の receiver::Voice。
//   入力: フレームごとの CELP ペイロード(158 bit を LSB first で 20 byte に詰めたもの。上位 2 bit は 0)、または欠落。
//   出力: 8 kHz・16 bit・モノラルの PCM、40 ms(320 sample)ごと。元のフレーム index を保つ。
//   先読み 7 フレーム(280 ms): 連続入力では 8 フレーム目で最初の出力が出る。flush() で残りを出す。
// 呼び出しのたびに read() で出力を空にすること(原本の C ABI と同じ受理規則: 未読の出力があると push は失敗する)。
// Qt / UHD / spear_core に依存しない。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

namespace spear::jr_train::voice {

using Payload = std::array<uint8_t, 20>;
using Pcm = std::array<int16_t, 320>;

struct Output {
    uint32_t index = 0;
    Pcm pcm{};
};

struct VoiceStatus {
    uint32_t finished = 0, failed = 0, synthesized = 0, output = 0;
    uint32_t last_decode_us = 0, maximum_frame_us = 0, maximum_flush_us = 0;
};

// 原本の Err(String) に相当する。メッセージは原本と同じ英文
class VoiceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Voice {
public:
    Voice();
    ~Voice();
    Voice(Voice&&) noexcept;
    Voice& operator=(Voice&&) noexcept;

    // index は単調増加。payload が無ければ欠落スロット(原本 missing_frame)。受理できなければ VoiceError
    void push(uint32_t index, const std::optional<Payload>& payload);
    std::optional<Output> read();
    void flush();
    const VoiceStatus& status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace spear::jr_train::voice
