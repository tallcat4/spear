// 選択チャネル 1 つの受信の全体(Qt / UHD 非依存。App・ツール・テストが同じものを使う):
//   radio.rx → Front(回転・÷40 → 100 kHz)→ Receiver(フレーム)→ Session(メッセージ・呼の状態)→ CallMonitor(送信の区切り・履歴)
//                                                                  └→ VoiceTimeline(TOKUDER の命令。復号は VoiceWorker)
// std-t99-receiver-sharp の native/t99.cpp(Context)と ReceiverWorker.cs をまとめたもの。並列受信(全チャネル同時)は持たない。
#pragma once

#include "call.hpp"
#include "front.hpp"
#include "receiver.hpp"
#include "session.hpp"
#include "voice/timeline.hpp"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace spear::std_t99 {

struct FrameEvent {
    const Frame& frame;
    const std::vector<Decoded>& decoded;
    const std::string& json;      // Session::json(burst は null)
    uint64_t rx_index;            // 同期語の先頭(radio.rx の index、provenance)
    bool new_call;                // このフレームで新しい送信が始まった
};

struct MonitorObserver {
    std::function<void(std::span<const cf32>)> band;          // 400 kHz(選択チャネルが DC)
    std::function<void(std::span<const cf32>)> channel;       // 100 kHz(Receiver の入力)
    std::function<void(std::span<const float>)> eye;          // 2 シンボル分のトレース(64 点)
    std::function<void(const FrameEvent&)> frame;
    std::function<void(const VoiceCommand&)> voice;
};

struct MonitorStatus {
    uint64_t frames = 0, crc_frames = 0;
    double power_db = -999, freq_offset_hz = 0;
    bool locked = false, have_offset = false;
};

class Monitor {
public:
    // in_rate: radio.rx のレート(100 kHz の整数倍)。shift_hz: 選択チャネル − 実際の LO
    Monitor(double in_rate, double shift_hz, MonitorObserver obs);
    // first_index: このブロックの先頭の radio.rx index
    void process(std::span<const cf32> in, uint64_t first_index);
    void flush_voice() { voice_.flush(); }
    // 待受(選択呼出、表 Annex 2-4-14)。再生の判定だけに使う(表示と保存は常にすべて)
    void set_standby(Standby mode, std::string own_id, int own_group) { standby_ = mode; own_id_ = std::move(own_id); own_group_ = own_group; }
    std::optional<bool> accepted() const;   // 現在の呼の判定(nullopt = 未確定 / 呼なし)
    const CallMonitor& calls() const { return calls_; }
    MonitorStatus status() const;
    double shift_hz() const { return front_.shift_hz(); }
    double band_rate() const { return front_.band_rate(); }
    double seconds() const { return static_cast<double>(out_) / kReceiverRate; }   // 受信した長さ

private:
    void on_frame(const Frame& f);
    uint64_t rx_index(uint64_t receiver_index) const;

    MonitorObserver obs_;
    Front front_;
    Receiver receiver_;
    Session session_;
    CallMonitor calls_;
    VoiceTimeline voice_;
    Standby standby_ = Standby::Monitor;
    std::string own_id_;
    int own_group_ = 0;
    std::vector<cf32> band_, out_buf_;
    uint64_t out_ = 0;               // Receiver に渡した 100 kHz の累計
    uint64_t rx0_ = 0;               // 最初のブロックの radio.rx index
    bool have_rx0_ = false;
    uint64_t eye_count_ = 0, last_frame_sample_ = 0;
    bool have_frame_ = false;
    double offset_hz_ = 0;
    bool have_offset_ = false;
};

ReceiverConfig receiver_config();   // 100 kHz / PFB 8(std-t99-receiver-sharp と同じ)

} // namespace spear::std_t99
