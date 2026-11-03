// ARIB STD-T99 Annex 2 4.2 control messages, as far as a listener needs them:
// who is talking (identification code / device ID), to whom, call type, emergency flags,
// position (GPS data 1, also carried by every PICH2), GPS data 2 and the caller name.
// Field offsets count from octet 1 bit 8 (the first transmitted bit), Figure Annex 2-4-27.
#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spear::std_t99 {

enum MessageType : int {
    kVoice = 0b00001, kEndCall = 0b00010, kIdle = 0b00100, kRemoteRequest = 0b01000,
    kLocationRequest = 0b01001, kGpsData1 = 0b01010, kGpsData2 = 0b01011,
    kDeviceIdRequest = 0b01100, kCallerName = 0b01101,
};

// Device ID (4.2.5.3): manufacturer 7 bit + individual ID 21 bit, displayed "MMM-IIIIIII"
struct DeviceId {
    int maker = 0;
    uint32_t individual = 0;
    static DeviceId from28(uint32_t raw) { return {static_cast<int>(raw >> 21), raw & 0x1fffffu}; }
    std::string str() const;
    bool zero() const { return maker == 0 && individual == 0; }
    bool operator==(const DeviceId&) const = default;
};

// PICH1 of a synchronous burst (Figure Annex 2-4-28): identification code (48) + called number (28) + 4 bits
struct Identification {
    uint32_t extension = 0;   // manufacturer-defined 20 bit
    DeviceId device;          // = caller number
    DeviceId called;
    int voice_additional = 0;
};
Identification parse_identification(const Bits& pich1);

struct Position {             // GPS data 1 fields (4.2.4.6, 4.8)
    uint32_t latitude = 0;    // ddmmmmmm  = ddmm.mmmm north
    uint32_t longitude = 0;   // dddmmmmmm = dddmm.mmmm east
    int altitude_m = 0;
    bool valid = false;       // A/V: 0 = A (valid)
    double latitude_deg() const;
    double longitude_deg() const;
    std::string str() const;  // "N35°40.2565' E139°45.0235' 7 m A"
};

struct GpsTime {              // GPS data 2 fields (4.2.4.7, 4.8), JST
    uint32_t time = 0;        // hhmmss
    uint32_t date = 0;        // ddmmyy
    int speed_01kn = 0;       // 0.1 knot
    int direction_deg = 0;
    std::string str() const;  // "2018-07-26 23:51:20 JST 2.7 kn 267°"
};

struct Message {
    int type = -1;            // 5-bit message type; >= 16 = manufacturer format 2
    int comm_type = -1;       // 001 group, 100 individual
    int call_control = -1;    // 00 normal, 01 emergency 1, 10 emergency 2, 11 response
    int voice_mode = -1;      // transmission mode, 000 = TOKUDER
    std::optional<DeviceId> caller, called;
    std::optional<Position> position;
    std::optional<GpsTime> gps_time;
    int maker = -1;           // idle / manufacturer format 2
    int name_part = -1;       // caller name: 0 = 1/2, 1 = 2/2
    std::vector<uint8_t> name_bytes;
    uint32_t name_check = 0;
    Bits bits;
};
// bits: 72 (SACCH super frame), 80 (FACCH/PICH) or 18 (single-frame SACCH unit)
Message parse_message(const Bits& bits);
// PICH2: communication type, message type and always the GPS data 1 layout (Figure Annex 2-4-28)
Message parse_pich2(const Bits& pich2);

const char* message_name(int type);
const char* comm_type_name(int comm_type);
const char* call_control_name(int call_control);
const char* voice_mode_name(int mode);
// Shift_JIS (caller name, 4.2.4.9) -> UTF-8; undecodable bytes become \xHH
std::string sjis_to_utf8(const std::vector<uint8_t>& bytes);

} // namespace spear::std_t99
