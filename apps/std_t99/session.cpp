#include "session.hpp"

#include <cstdio>
#include <sstream>

namespace spear::std_t99 {

std::string json_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

void Session::apply(const Message& m) {
    if (m.type < 0) return;
    if (m.type != kIdle) state_.last_type = m.type;
    if (m.comm_type == 0b001 || m.comm_type == 0b100) state_.comm_type = m.comm_type;
    if (m.call_control >= 0) state_.call_control = m.call_control;
    if (m.voice_mode >= 0) state_.voice_mode = m.voice_mode;
    if (m.caller && !m.caller->zero()) state_.caller = m.caller;
    if (m.called) state_.called = m.called;
    if (m.position) state_.position = m.position;
    if (m.gps_time) state_.gps_time = m.gps_time;
    if (m.type == kEndCall) state_.ended = true;
    else if (m.type == kVoice) state_.ended = false;
    if (m.type == kCallerName) {
        if (m.name_part == 0) name_first_ = m.name_bytes;
        else if (m.name_part == 1 && name_first_.size() == 8) {
            std::vector<uint8_t> name = name_first_;
            name.insert(name.end(), m.name_bytes.begin(), m.name_bytes.end());
            state_.name_check_failed = crc32_mpeg2(name) != m.name_check;
            if (!state_.name_check_failed) {
                while (!name.empty() && (name.back() == 0x00 || name.back() == 0x20)) name.pop_back();
                state_.name = sjis_to_utf8(name);
            }
            name_first_.clear();
        }
    }
}

std::vector<Decoded> Session::update(const Frame& f) {
    std::vector<Decoded> out;
    auto add = [&](std::string channel, Message m) {
        apply(m);
        out.push_back({std::move(channel), std::move(m)});
    };
    // A synchronous burst starts a new transmission: forget the previous caller's details
    if (f.sync_burst() && f.halves[0].control && f.halves[0].control->crc_ok) {
        state_ = {};
        name_first_.clear();
    }
    // SACCH: W carries the group number in every unit; the information is a super frame (M=10) or one unit (M=01)
    if (f.sacch.crc_ok) {
        state_.group = f.sacch.group;
        if (f.rich.m == 0b10) {
            if (f.sacch.structure == 0b11) { superframe_.clear(); expected_structure_ = 0b11; }
            if (f.sacch.structure == expected_structure_ && (superframe_.size() == static_cast<std::size_t>(3 - expected_structure_))) {
                superframe_.push_back(f.sacch.unit);
                expected_structure_ = f.sacch.structure - 1;
                if (superframe_.size() == 4) {
                    Bits message;
                    for (const auto& unit : superframe_) message.insert(message.end(), unit.begin(), unit.end());
                    add("SACCH(SF)", parse_message(message));
                    superframe_.clear();
                    expected_structure_ = 0b11;
                }
            } else { superframe_.clear(); expected_structure_ = 0b11; }
        } else {
            superframe_.clear();
            expected_structure_ = 0b11;
            add("SACCH", parse_message(f.sacch.unit));
        }
    } else if (f.rich.m == 0b10) { superframe_.clear(); expected_structure_ = 0b11; }

    for (int i = 0; i < 2; ++i) {
        const auto& h = f.halves[static_cast<std::size_t>(i)];
        if (!h.control || !h.control->crc_ok) continue;
        const auto& bits = h.control->message;
        if (h.kind == HalfKind::pich && i == 0) {
            const auto id = parse_identification(bits);
            state_.identification = id;
            if (!id.device.zero()) state_.caller = id.device;
            state_.called = id.called;
            Message m;
            m.bits = bits;
            m.caller = id.device;
            m.called = id.called;
            out.push_back({"PICH1", std::move(m)});
        } else if (h.kind == HalfKind::pich) add("PICH2", parse_pich2(bits));
        else add(i == 0 ? "FACCH1" : "FACCH2", parse_message(bits));
    }
    return out;
}

namespace {
const char* half_name(HalfKind k) { return k == HalfKind::tch ? "TCH" : k == HalfKind::pich ? "PICH" : "FACCH"; }
std::string hex_bytes(const uint8_t* p, std::size_t n) {
    static constexpr char d[] = "0123456789ABCDEF";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}
std::string message_json(const Decoded& d) {
    const auto& m = d.message;
    std::ostringstream o;
    o << "{\"ch\":\"" << d.channel << "\"";
    if (d.channel == "PICH1") {
        const auto id = parse_identification(m.bits);
        o << ",\"type\":\"Identification\",\"extension\":" << id.extension << ",\"caller\":\"" << id.device.str()
          << "\",\"called\":\"" << id.called.str() << "\",\"voice_additional\":" << id.voice_additional;
    } else {
        o << ",\"type\":\"" << message_name(m.type) << "\",\"type_code\":" << m.type;
        if (m.comm_type >= 0) o << ",\"comm\":\"" << comm_type_name(m.comm_type) << "\"";
        if (m.caller) o << ",\"caller\":\"" << m.caller->str() << "\"";
        if (m.called) o << ",\"called\":\"" << m.called->str() << "\"";
        if (m.call_control >= 0) o << ",\"call_control\":\"" << call_control_name(m.call_control) << "\"";
        if (m.voice_mode >= 0) o << ",\"voice_mode\":\"" << voice_mode_name(m.voice_mode) << "\"";
        if (m.position) {
            const auto& p = *m.position;
            char b[96];
            std::snprintf(b, sizeof b, "%.7f,\"lon\":%.7f", p.latitude_deg(), p.longitude_deg());
            o << ",\"lat\":" << b << ",\"alt_m\":" << p.altitude_m << ",\"gps_valid\":" << (p.valid ? "true" : "false")
              << ",\"lat_raw\":" << p.latitude << ",\"lon_raw\":" << p.longitude;
        }
        if (m.gps_time) {
            const auto& g = *m.gps_time;
            o << ",\"gps_time\":\"" << g.str() << "\",\"time_raw\":" << g.time << ",\"date_raw\":" << g.date
              << ",\"speed_01kn\":" << g.speed_01kn << ",\"direction_deg\":" << g.direction_deg;
        }
        if (m.maker >= 0) o << ",\"maker\":" << m.maker;
        if (m.name_part >= 0) {
            o << ",\"name_part\":" << m.name_part + 1 << ",\"name_bytes\":\"" << hex_bytes(m.name_bytes.data(), m.name_bytes.size()) << "\"";
            if (m.name_part == 1) { char b[16]; std::snprintf(b, sizeof b, "%08X", m.name_check); o << ",\"name_check\":\"" << b << "\""; }
        }
    }
    o << ",\"bits\":\"" << to_hex(m.bits) << "\"}";
    return o.str();
}
}

std::string Session::json(const Frame& f, const std::vector<Decoded>& decoded, double seconds, int64_t burst) const {
    std::ostringstream o;
    char t[32];
    std::snprintf(t, sizeof t, "%.4f", seconds);
    o << "{\"t\":" << t << ",\"sample\":" << f.input_sample_index << ",\"symbol\":" << f.symbol_index
      << ",\"sync\":\"" << (f.sync_ok ? "ok" : "flywheel") << "\",\"sse\":" << static_cast<int>(f.sync_sse * 100) / 100.0
      << ",\"kind\":\"" << (f.sync_burst() ? "SB" : f.rich.f == 0b10 ? "SC" : "unknown") << "\""
      << ",\"rich\":{\"f\":" << f.rich.f << ",\"m\":" << f.rich.m << ",\"o\":" << f.rich.o << ",\"d\":" << f.rich.d
      << ",\"parity_ok\":" << (f.rich.parity_ok ? "true" : "false") << "}"
      << ",\"sacch\":{\"crc\":" << (f.sacch.crc_ok ? "true" : "false") << ",\"errors\":" << f.sacch.bit_errors
      << ",\"structure\":" << f.sacch.structure << ",\"group\":" << f.sacch.group << ",\"unit\":\"" << to_hex(f.sacch.unit) << "\"}"
      << ",\"halves\":[";
    for (int i = 0; i < 2; ++i) {
        const auto& h = f.halves[static_cast<std::size_t>(i)];
        if (i) o << ',';
        o << "{\"kind\":\"" << half_name(h.kind) << "\"";
        if (h.control) o << ",\"crc\":" << (h.control->crc_ok ? "true" : "false") << ",\"errors\":" << h.control->bit_errors
                         << ",\"message\":\"" << to_hex(h.control->message) << "\"";
        else o << ",\"voice\":\"" << hex_bytes(h.payload.data(), 9) << ' ' << hex_bytes(h.payload.data() + 9, 9) << "\"";
        o << '}';
    }
    o << "],\"messages\":[";
    for (std::size_t i = 0; i < decoded.size(); ++i) o << (i ? "," : "") << message_json(decoded[i]);
    o << "]";
    if (!state_.name.empty()) o << ",\"name\":\"" << json_escape(state_.name) << "\"";
    o << ",\"burst\":";
    if (burst >= 0) o << burst; else o << "null";
    o << ",\"raw\":{\"sw\":\"" << to_hex(f.fields.sw) << "\",\"rich\":\"" << to_hex(f.fields.rich) << "\",\"sacch\":\""
      << to_hex(f.fields.sacch) << "\",\"first\":\"" << to_hex(f.fields.first) << "\",\"second\":\"" << to_hex(f.fields.second) << "\"}}";
    return o.str();
}

} // namespace spear::std_t99
