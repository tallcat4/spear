#include "monitor.hpp"

#include <chrono>
#include <cmath>

namespace spear::std_t99 {

namespace {
constexpr uint64_t kRfFrame = 8000;   // 80 ms at 100 kHz
double utc_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}

ReceiverConfig receiver_config() {
    ReceiverConfig c;
    c.in_rate = kReceiverRate;
    c.pfb_channels = 8;
    return c;
}

Monitor::Monitor(double in_rate, double shift_hz, MonitorObserver obs)
    : obs_(std::move(obs)), front_(in_rate, shift_hz), receiver_(receiver_config()),
      voice_([this](const VoiceCommand& c) { if (obs_.voice) obs_.voice(c); }) {
    receiver_.set_eye_handler([this](std::span<const float> values) {
        // 600 トレース/s(2400 シンボルのうち 4 つに 1 つ)。同期中か、直近 6 フレーム以内にフレームが出ている間だけ
        if ((eye_count_++ % 4) != 0 || !obs_.eye) return;
        if (!(receiver_.metrics().locked || (have_frame_ && out_ < last_frame_sample_ + kRfFrame * 6))) return;
        obs_.eye(values);
    });
    receiver_.set_frame_handler([this](const Frame& f) { on_frame(f); });
}

uint64_t Monitor::rx_index(uint64_t receiver_index) const {
    return rx0_ + static_cast<uint64_t>(std::max(0.0, front_.input_index(static_cast<double>(receiver_index))));
}

void Monitor::process(std::span<const cf32> in, uint64_t first_index) {
    if (!have_rx0_) { rx0_ = first_index; have_rx0_ = true; }
    front_.process(in, band_, out_buf_);
    if (obs_.band && !band_.empty()) obs_.band(band_);
    if (out_buf_.empty()) return;
    if (obs_.channel) obs_.channel(out_buf_);
    receiver_.process(out_buf_, out_);
    out_ += out_buf_.size();
    // 入力は回さない: 受信機がフレームごとに同期語の平均(DC)で搬送波のずれを差し引く(±700 Hz 程度)。同期中に測ったずれは表示用に残す
    const auto& m = receiver_.metrics();
    if (m.locked && m.have_offset) { offset_hz_ = m.freq_offset_hz; have_offset_ = true; }
    voice_.idle(out_);
}

void Monitor::on_frame(const Frame& f) {
    last_frame_sample_ = f.input_sample_index;
    have_frame_ = true;
    const auto decoded = session_.update(f);
    const auto idx = rx_index(f.input_sample_index);
    const bool new_call = calls_.frame(f, session_, decoded, utc_now(), idx);
    const std::string json = session_.json(f, decoded, static_cast<double>(f.input_sample_index) / kReceiverRate, -1);
    if (obs_.frame) obs_.frame(FrameEvent{f, decoded, json, idx, new_call});
    // 音声: 同期バーストか終話で流れを終える。通信チャネルのフレームが流れを作る。再生は待受の判定(保存は常に)
    bool end_call = false;
    for (const auto& d : decoded) end_call |= d.message.type == kEndCall;
    voice_.frame(f, end_call, accepted().value_or(false));
}

std::optional<bool> Monitor::accepted() const {
    if (!calls_.current()) return std::nullopt;
    return accepts(*calls_.current(), standby_, own_id_, own_group_);
}

MonitorStatus Monitor::status() const {
    const auto& m = receiver_.metrics();
    MonitorStatus s;
    s.frames = m.frames;
    s.crc_frames = m.crc_frames;
    s.power_db = m.power_db;
    s.locked = m.locked;
    s.freq_offset_hz = offset_hz_;
    s.have_offset = have_offset_;
    return s;
}

} // namespace spear::std_t99
