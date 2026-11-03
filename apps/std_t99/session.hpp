// Collects what the frames say about the current transmission: SACCH super-frame
// reassembly (4 x 18 bit -> 72-bit message), caller-name reassembly and CRC check,
// and the latest identity / position / GPS time. Produces one JSON line per frame.
#pragma once

#include "protocol/messages.hpp"
#include "receiver.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spear::std_t99 {

struct Decoded {             // one message decoded in this frame
    std::string channel;     // "SACCH", "SACCH(SF)", "PICH1", "PICH2", "FACCH1", "FACCH2"
    Message message;
};

struct CallState {
    std::optional<Identification> identification;   // from PICH1
    std::optional<DeviceId> caller, called;
    int comm_type = -1, group = -1, call_control = -1, voice_mode = -1;
    int last_type = -1;
    std::optional<Position> position;
    std::optional<GpsTime> gps_time;
    std::string name;            // UTF-8, empty until both parts arrive with a valid check
    bool name_check_failed = false;
    bool ended = false;          // "End call" received
};

class Session {
public:
    // Returns the messages decoded in this frame and updates state().
    std::vector<Decoded> update(const Frame& frame);
    const CallState& state() const { return state_; }
    // One JSON object (no newline). burst: index in .burst, or -1 when the frame was not written there.
    std::string json(const Frame& frame, const std::vector<Decoded>& decoded, double seconds, int64_t burst) const;

private:
    void apply(const Message& m);
    CallState state_;
    std::vector<Bits> superframe_;          // units collected so far (structure 11, 10, 01, 00)
    int expected_structure_ = 0b11;
    std::vector<uint8_t> name_first_;       // caller name 1/2 bytes, waiting for 2/2
};

std::string json_escape(const std::string& s);

} // namespace spear::std_t99
