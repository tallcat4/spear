// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 復号した音声と、フレーム・電文の保存。原本: crates/receiver-core/src/recording.rs と wave.rs(PcmWave)(commit dfc7440)。
//   <親>/recording-YYYYMMDDTHHMMSS.sssZ-NNN/
//     session.json        設定(保存内容、チャネル、時刻の意味)
//     <周波数>Hz-NNNNNN.wav 8 kHz・16 bit・モノラル。最初の音声から始まり、5 s 以下の欠落は無音で埋め、5 s 超・再同期・30 分で分割
//     files.jsonl         確定した WAV ごとの周波数・epoch・フレーム番号の範囲
//     raw-frames.jsonl    48 byte の無線フレーム(デスクランブル・訂正前)。選択時
//     data-messages.jsonl 復号した電文。選択時
//     result.json         終了時の件数とエラー
//     INCOMPLETE          保存中・異常終了の目印(正常に終われば消す)
// 書き込みは専用スレッドと有界キュー(2048)。追いつかなければ保存をエラーで終え、受信は止めない。
#pragma once

#include "pipeline.hpp"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace spear::jr_train {

struct OutputOptions {
    bool audio = true, raw_frames = false, data_messages = false;
};

struct RecordingStatus {
    std::string state = "idle";   // idle / recording / stopping / completed / failed
    std::optional<std::string> directory, started_at, finished_at;
    uint64_t files = 0, pcm_samples = 0, raw_frames = 0, data_messages = 0;
    OutputOptions options;
    std::optional<std::string> error;
};

class Recorder {
public:
    Recorder() = default;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    RecordingStatus snapshot() const;
    // 失敗は std::runtime_error(原本と同じ日本語のメッセージ)
    RecordingStatus start(const std::filesystem::path& parent, const std::vector<ChannelConfig>& channels, OutputOptions options = {});
    void push(const Audio& audio);
    void push_frame(const std::string& channel, const FrameEvent& event);
    RecordingStatus stop();

    struct FrameItem {
        std::string channel, received_at;
        FrameEvent event;
    };
    using Event = std::variant<Audio, FrameItem>;

private:
    void send(Event e);
    mutable std::mutex status_mu_;
    RecordingStatus status_;
    std::mutex op_mu_;   // start / stop の直列化
    std::thread thread_;
    // キュー(生産者 = 受信のスレッド、消費者 = 書き込みスレッド)
    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<Event> queue_;
    bool sending_ = false, closed_ = true;
    OutputOptions options_;
};

// 8 kHz・16 bit・モノラルの WAV(原本 PcmWave)。ヘッダの長さは finish() で確定する
class PcmWave {
public:
    explicit PcmWave(const std::filesystem::path& path);   // 既存のファイルは上書きしない(失敗は std::runtime_error)
    void write(const int16_t* pcm, std::size_t n);
    void silence(uint64_t samples);
    uint64_t samples() const { return samples_; }
    void finish();

private:
    void header();
    std::filesystem::path path_;
    std::FILE* f_ = nullptr;
    uint64_t samples_ = 0;
};

} // namespace spear::jr_train
