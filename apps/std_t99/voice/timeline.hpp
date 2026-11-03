// TOKUDER の音声の流れ(DSP thread 側)。std-t99-receiver-sharp native/t99.cpp の Voice を、復号器を持たない形にしたもの。
// フレームから「48 bit を 1 つ復号する」「流れを終える」の命令を順に出し、復号は VoiceWorker(専用スレッド)が行う
// (docs/DEVELOPMENT.md: 推論は DSP スレッドの外)。命令の順序が原本の Decoder の呼び出し順と同じなので、出る音声も同じ。
//
// 時間軸: RF フレーム(80 ms)あたり TCH 4 スロット。流れの最初の音声フレームのスロット 0 が 0。
// 送信の途中の FACCH の半分と受信できなかった RF フレームは消失スロット(直前のフレームを繰り返す。tokuder_neurodec
// frames.to_info48 と同じ)。長い途切れ(480 ms 超)、同期バースト、終話で流れを区切る。
#pragma once

#include "../receiver.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace spear::std_t99 {

struct VoiceCommand {
    enum class Kind { Frame, Flush } kind = Kind::Frame;
    std::array<uint8_t, 48> info{};   // Frame: 誤り訂正後の 48 bit
    bool play = true;                 // Frame: 再生するか(待受の判定。保存は常に行う)
};

class VoiceTimeline {
public:
    using Sink = std::function<void(const VoiceCommand&)>;
    explicit VoiceTimeline(Sink sink) : sink_(std::move(sink)) {}
    // 受信したフレームごとに呼ぶ(Receiver の frame handler の順)。end_call は Session がこのフレームで終話を復号したか、
    // play は待受の判定(このフレームの時点)
    void frame(const Frame& f, bool end_call, bool play);
    // フレームが来ない間も入力の進み(Receiver の 100 kHz index)で呼ぶ: 途切れが長ければ流れを終える
    void idle(uint64_t sample);
    void flush();
    bool active() const { return active_; }

private:
    void stream_frame(const Frame& f, bool play);
    void push(const uint8_t* info, bool play);
    void erased(int64_t slots, bool play) { for (int64_t i = 0; i < slots; ++i) push(previous_.data(), play); }
    int64_t number(uint64_t sample) const;

    Sink sink_;
    bool active_ = false;
    uint64_t origin_ = 0;                        // 流れの最初のフレームの同期語の sample
    int64_t last_frame_ = 0;                     // origin_ からの RF フレーム番号
    std::array<uint8_t, 48> previous_{};
    bool last_play_ = true;
    static constexpr int64_t kMaxGap = 6;        // 消失スロットで埋める RF フレーム数(480 ms)
};

} // namespace spear::std_t99
