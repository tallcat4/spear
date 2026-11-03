#include "messages.hpp"

#include <cstdio>

#include <iconv.h>

namespace spear::std_t99 {
namespace {
uint32_t u32(const Bits& b, std::size_t first, std::size_t count) { return static_cast<uint32_t>(field(b, first, count)); }
int i32(const Bits& b, std::size_t first, std::size_t count) { return static_cast<int>(field(b, first, count)); }
std::string format(const char* pattern, auto... values) {
    char buffer[128];
    std::snprintf(buffer, sizeof buffer, pattern, values...);
    return buffer;
}
}

std::string DeviceId::str() const { return format("%03d-%07u", maker, individual); }

Identification parse_identification(const Bits& b) {
    Identification id;
    if (b.size() < 80) return id;
    id.extension = u32(b, 0, 20);
    id.device = {i32(b, 20, 7), u32(b, 27, 21)};
    id.called = DeviceId::from28(u32(b, 48, 28));
    id.voice_additional = i32(b, 76, 4);
    return id;
}

double Position::latitude_deg() const { return latitude / 1000000 + (latitude % 1000000) / 10000.0 / 60.0; }
double Position::longitude_deg() const { return longitude / 1000000 + (longitude % 1000000) / 10000.0 / 60.0; }
std::string Position::str() const {
    return format("N%u°%02u.%04u' E%u°%02u.%04u' %d m %s", latitude / 1000000, latitude / 10000 % 100, latitude % 10000,
                  longitude / 1000000, longitude / 10000 % 100, longitude % 10000, altitude_m, valid ? "A" : "V(invalid)");
}

std::string GpsTime::str() const {
    if (date == 0) return format("no fix (date 0)  %d.%d kn  %d deg", speed_01kn / 10, speed_01kn % 10, direction_deg);
    return format("20%02u-%02u-%02u %02u:%02u:%02u JST  %d.%d kn  %d deg", date % 100, date / 100 % 100, date / 10000,
                  time / 10000, time / 100 % 100, time % 100, speed_01kn / 10, speed_01kn % 10, direction_deg);
}

namespace {
Position position_at(const Bits& b) {   // offsets of Figure Annex 2-4-34
    Position p;
    p.latitude = u32(b, 8, 26);
    p.longitude = u32(b, 34, 28);
    p.altitude_m = i32(b, 62, 13);
    p.valid = b[75] == 0;
    return p;
}
}

Message parse_message(const Bits& b) {
    Message m;
    m.bits = b;
    if (b.size() < 18) return m;
    m.type = i32(b, 3, 5);
    if (m.type & 0b10000) {   // manufacturer identification message format 2
        m.comm_type = i32(b, 0, 3);
        m.maker = i32(b, 8, 7);
        return m;
    }
    switch (m.type) {
    case kIdle:
        m.maker = i32(b, 8, 7);
        return m;
    case kCallerName:
        m.name_part = b[0];
        if (b.size() < 72) return m;
        if (m.name_part == 0) for (int i = 0; i < 8; ++i) m.name_bytes.push_back(static_cast<uint8_t>(field(b, 8 + 8 * i, 8)));
        else {
            for (int i = 0; i < 4; ++i) m.name_bytes.push_back(static_cast<uint8_t>(field(b, 8 + 8 * i, 8)));
            m.name_check = u32(b, 40, 32);
        }
        return m;
    default: break;
    }
    if (b.size() < 72) return m;
    m.comm_type = i32(b, 0, 3);
    switch (m.type) {
    case kVoice: case kEndCall: case kRemoteRequest: case kLocationRequest: case kDeviceIdRequest:
        m.caller = DeviceId::from28(u32(b, 8, 28));
        m.called = DeviceId::from28(u32(b, 36, 28));
        m.call_control = i32(b, 64, 2);
        if (m.type == kVoice) m.voice_mode = i32(b, 69, 3);
        break;
    case kGpsData1:
        if (b.size() < 80) break;
        m.position = position_at(b);
        m.call_control = i32(b, 78, 2);
        break;
    case kGpsData2:
        m.gps_time = GpsTime{u32(b, 8, 18), u32(b, 26, 20), i32(b, 46, 12), i32(b, 58, 12)};
        m.call_control = i32(b, 70, 2);
        break;
    default: break;
    }
    return m;
}

Message parse_pich2(const Bits& b) {
    Message m;
    m.bits = b;
    if (b.size() < 80) return m;
    m.comm_type = i32(b, 0, 3);
    m.type = i32(b, 3, 5);
    m.position = position_at(b);
    m.call_control = i32(b, 78, 2);
    return m;
}

const char* message_name(int type) {
    switch (type) {
    case kVoice: return "Voice";
    case kEndCall: return "End call";
    case kIdle: return "Idle";
    case kRemoteRequest: return "Remote TX request";
    case kLocationRequest: return "Location request";
    case kGpsData1: return "GPS data 1";
    case kGpsData2: return "GPS data 2";
    case kDeviceIdRequest: return "Device ID request";
    case kCallerName: return "Caller name";
    default: return type >= 16 ? "Maker format 2" : type < 0 ? "-" : "Reserved";
    }
}
const char* comm_type_name(int c) { return c == 0b001 ? "Group" : c == 0b100 ? "Individual" : c < 0 ? "-" : "Reserved"; }
const char* call_control_name(int c) {
    switch (c) {
    case 0: return "Normal";
    case 1: return "EMERGENCY 1";
    case 2: return "EMERGENCY 2";
    case 3: return "Emergency response";
    default: return "-";
    }
}
const char* voice_mode_name(int mode) { return mode == 0 ? "TOKUDER" : mode < 0 ? "-" : "Undefined"; }

std::string sjis_to_utf8(const std::vector<uint8_t>& bytes) {
    std::string out;
    // SPEAR: Windows の MultiByteToWideChar(932) の代わりに glibc の iconv(CP932)。変換できなければ下の \xHH 表記
    if (!bytes.empty()) {
        if (iconv_t cd = iconv_open("UTF-8", "CP932"); cd != reinterpret_cast<iconv_t>(-1)) {
            std::string in(bytes.begin(), bytes.end());
            std::string buf(bytes.size() * 4 + 4, '\0');
            char* ip = in.data();
            char* op = buf.data();
            std::size_t il = in.size(), ol = buf.size();
            const bool ok = iconv(cd, &ip, &il, &op, &ol) != static_cast<std::size_t>(-1) && il == 0;
            iconv_close(cd);
            if (ok) { buf.resize(buf.size() - ol); return buf; }
        }
    }
    for (uint8_t c : bytes) {
        if (c >= 0x20 && c < 0x7f) out.push_back(static_cast<char>(c));
        else out += format("\\x%02X", c);
    }
    return out;
}

} // namespace spear::std_t99
