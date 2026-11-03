// 受信の保存(std-t99-receiver-sharp の Recording、FORMAT.md と同じ形式)。同じ名前 <dir>/T99-YYYYMMDD-HHMMSS-CHnn で 4 つ:
//   .burst  TCH/TCH のフレームだけ、1 レコード 41 byte(FF + (48 + 9 byte) × 4。std-t98 系と同じ)
//   .llr    .burst と対応する軟判定、1 レコード 288 byte(4 × 72 個の int8、正 = 1)
//   .jsonl  全フレーム。Session::json に utc(受信した PC の時刻)と burst(.burst のレコード番号)を入れたもの
//   .wav    8 kHz / PCM16 / モノラル。復号した音声を受信順に連結(送信間の無信号は保持しない。待受の設定によらず全通話)
// .jsonl と .llr を rf.jsonl / rf.llr にして 1 つのフォルダーに置くと tokuder-neurodec の tokuder2wav で再処理できる。
// フレームは DSP thread、音声は復号スレッドから書くので mutex で守る(1 フレーム 80 ms に数百 byte なので書き込みは軽い)。
#pragma once

#include "receiver.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>

namespace spear::std_t99 {

class Recorder {
public:
    ~Recorder() { stop(); }
    // 失敗は std::runtime_error。既存のファイルは上書きしない
    std::string start(const std::filesystem::path& dir, int channel);
    void stop();
    bool active() const { std::lock_guard<std::mutex> lk(mu_); return open_; }
    // json: Session::json の出力(burst は null のまま渡す。ここで .burst の番号に置き換える)
    void frame(const Frame& f, const std::string& json, const std::string& utc_iso);
    void audio(std::span<const float> pcm);
    std::string stem() const { std::lock_guard<std::mutex> lk(mu_); return stem_; }
    uint64_t frames() const { std::lock_guard<std::mutex> lk(mu_); return frames_; }
    uint64_t audio_samples() const { std::lock_guard<std::mutex> lk(mu_); return samples_; }

private:
    void write_wav_header();
    mutable std::mutex mu_;
    bool open_ = false;
    std::string stem_;
    std::ofstream burst_, llr_, jsonl_, wav_;
    uint64_t records_ = 0, frames_ = 0, samples_ = 0;
};

} // namespace spear::std_t99
