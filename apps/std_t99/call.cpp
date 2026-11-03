#include "call.hpp"

#include <algorithm>
#include <cstdio>

namespace spear::std_t99 {

std::string CallInfo::call_type() const {
    if (comm_type == 4) return "INDIVIDUAL";
    if (comm_type == 1) return group == 0 ? "ALL CALL" : group > 0 ? "GROUP" : "GROUP/ALL";
    return "";
}

std::string CallInfo::destination() const {
    if (comm_type == 4) return called.value_or("");
    if (comm_type == 1) {
        if (group == 0) return "ALL";
        if (group > 0) { char b[32]; std::snprintf(b, sizeof b, "GROUP %02d", group); return b; }
        return "";
    }
    return called.value_or("");
}

std::optional<bool> accepts(const CallInfo& call, Standby mode, const std::string& own_id, int own_group) {
    if (mode == Standby::Monitor) return true;
    if (call.comm_type == 4) {   // 個別通話: 宛先 = 自局機器 ID
        if (!call.called) return std::nullopt;
        return *call.called == own_id;
    }
    if (call.comm_type != 1 || call.group < 0) return std::nullopt;
    if (mode == Standby::Individual) return false;   // グループ・一斉通話は受けない(表の注 2)
    if (call.group == 0) return true;                // 一斉通話
    return own_group == 0 || own_group == call.group;   // グループ待受 0 はすべてのグループ
}

std::optional<std::string> normalize_device_id(int maker, int64_t individual) {
    if (maker < 0 || maker > 127 || individual < 0 || individual > 2097151) return std::nullopt;
    char b[16];
    std::snprintf(b, sizeof b, "%03d-%07lld", maker, static_cast<long long>(individual));
    return std::string(b);
}

void CallMonitor::remember(const CallInfo& c) {
    if (!history_.empty() && history_.front().number == c.number) history_.front() = c;
    else history_.push_front(c);
    if (history_.size() > kHistory) history_.pop_back();
}

bool CallMonitor::frame(const Frame& f, const Session& session, const std::vector<Decoded>& decoded, double utc, uint64_t rx_index) {
    // 同期バーストの PICH1(CRC OK)で送信が始まる。途中から聞いたフレームも 1 つの呼にする。終話の後の同期バーストも新しい呼
    const bool new_call = f.sync_burst() && f.halves[0].control && f.halves[0].control->crc_ok;
    bool started = false;
    if (!call_ || new_call || (call_->ended && f.sync_burst())) {
        if (call_) remember(*call_);
        call_ = CallInfo{};
        call_->number = ++count_;
        call_->started_utc = utc;
        call_->first_index = rx_index;
        started = true;
    }
    auto& c = *call_;
    const auto& s = session.state();
    c.last_utc = utc;
    c.last_index = rx_index;
    ++c.frames;
    if (s.caller) c.caller = s.caller->str();
    if (s.called) c.called = s.called->str();
    if (s.identification) c.extension = s.identification->extension;
    if (s.comm_type >= 0) c.comm_type = s.comm_type;
    if (s.group >= 0) c.group = s.group;
    if (s.call_control >= 0) c.call_control = s.call_control;
    if (s.voice_mode >= 0) c.voice_mode = s.voice_mode;
    if (!s.name.empty()) c.name = s.name;
    c.name_failed = s.name_check_failed;
    c.ended = s.ended;
    c.voice = c.voice || f.halves[0].kind == HalfKind::tch || f.halves[1].kind == HalfKind::tch;
    if (s.position) c.position = s.position;
    if (s.gps_time) c.gps_time = s.gps_time;
    for (const auto& d : decoded) {
        if (d.channel == "PICH2") c.kind = d.message.type;
        const int t = d.message.type;
        if ((t == kRemoteRequest || t == kLocationRequest || t == kDeviceIdRequest) && std::find(c.requests.begin(), c.requests.end(), t) == c.requests.end())
            c.requests.push_back(t);
    }
    remember(c);
    return started;
}

} // namespace spear::std_t99
