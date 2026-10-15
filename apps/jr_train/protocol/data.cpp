// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/receiver-core/src/data/{mod.rs, packet.rs, operation.rs}
#include "data.hpp"

#include "coding.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>

namespace spear::jr_train::protocol {
namespace {

std::string hex(const std::vector<uint8_t>& b, std::size_t from, std::size_t to) { return hex_upper(b.data() + from, to - from); }
template <std::size_t N>
std::string hex(const std::array<uint8_t, N>& b, std::size_t from, std::size_t to) { return hex_upper(b.data() + from, to - from); }

std::string pairs(const uint8_t* b, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; i += 2) {
        if (i) out += ' ';
        out += hex_upper(b + i, std::min<std::size_t>(2, n - i));
    }
    return out;
}
uint16_t be(const uint8_t* b) { return static_cast<uint16_t>((b[0] << 8) | b[1]); }
uint16_t sum(const uint8_t* b, std::size_t n) {
    uint16_t s = 0;
    for (std::size_t i = 0; i < n; ++i) s = static_cast<uint16_t>(s + b[i]);
    return s;
}
std::optional<uint32_t> bcd(const uint8_t* b, std::size_t n) {
    uint32_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if ((b[i] >> 4) > 9 || (b[i] & 15) > 9) return std::nullopt;
        v = v * 100 + static_cast<uint32_t>(b[i] >> 4) * 10 + static_cast<uint32_t>(b[i] & 15);
    }
    return v;
}
// 列車番号: BCD 2 byte + 英字(1..26 → A..Z)
std::optional<std::string> train(const uint8_t* b) {
    if (b[2] < 1 || b[2] > 26) return std::nullopt;
    const auto n = bcd(b, 2);
    if (!n) return std::nullopt;
    return std::to_string(*n) + static_cast<char>(64 + b[2]);
}
bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
// BCD の YYMMDDhh mm [ss] → "YYYY-MM-DDThh:mm:ss"(原本は chrono で妥当性を確かめる)
std::string stamp(const uint8_t* b, std::size_t n) {
    if (n != 5 && n != 6) throw DataError("Invalid date length");
    uint32_t v[6] = {};
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = bcd(b + i, 1);
        if (!x) throw DataError("Invalid BCD date");
        v[i] = *x;
    }
    const int year = 2000 + static_cast<int>(v[0]);
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (v[1] < 1 || v[1] > 12 || v[2] < 1 || static_cast<int>(v[2]) > kDays[v[1] - 1] + (v[1] == 2 && leap(year) ? 1 : 0)) throw DataError("Invalid message date");
    if (v[3] > 23 || v[4] > 59 || v[5] > 59) throw DataError("Invalid message time");
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02u:%02u:%02u", year, v[1], v[2], v[3], v[4], v[5]);
    return buf;
}
void add(std::vector<Field>& f, const std::string& name, const std::string& value) { f.push_back({name, value, std::nullopt}); }
void add(std::vector<Field>& f, const std::string& name, uint64_t value) { add(f, name, std::to_string(value)); }
template <class T>
void optional(std::vector<Field>& f, const std::string& name, const std::optional<T>& v) {
    if (!v) return;
    if constexpr (std::is_same_v<T, std::string>) add(f, name, *v);
    else add(f, name, static_cast<uint64_t>(*v));
}
void identity(std::vector<Field>& f, const uint8_t* b) {
    add(f, "ClassCode", hex_upper(b, 2));
    add(f, "Formation", hex_upper(b + 2, 1));
}
Message message(const std::string& kind, const std::string& id, const Packet& p) {
    Message m;
    m.kind = kind;
    m.identity = id;
    m.raw_hex = hex_upper(p.bytes.data(), p.bytes.size());
    m.first_bit = p.first;
    m.last_bit = p.last;
    return m;
}
Message unknown(const Packet& p) { return message("Unsupported", hex_upper(p.bytes.data(), 1), p); }

Message traffic(const Packet& p) {
    const auto& b = p.bytes;
    const std::size_t n = b.size();
    std::vector<Field> f;
    std::string kind, id;
    std::optional<std::string> date;
    if (n == 18 && b[0] == 0x51 && b[1] == 0x10 && b[2] == 0) {
        identity(f, &b[3]);
        add(f, "UnassignedBytes", hex(b, 6, n));
        kind = "Presence";
        id = hex(b, 3, 6);
    } else if (n == 31 && b[0] == 5) {
        const uint8_t phase = b[4] & 127;
        identity(f, &b[9]);
        add(f, "Subtype", hex(b, 2, 3));
        add(f, "CabFlags", hex(b, 3, 4));
        add(f, "VehicleKind", hex(b, 12, 13));
        add(f, "LineCode", hex(b, 17, 18));
        add(f, "DispatchNotice", (b[4] & 128) != 0 ? 1u : 0u);
        add(f, "NumberPhase", phase);
        if (phase != 3) optional(f, "TrainNumber", train(&b[6]));
        if ((b[5] & 0xc0) == 0x80) add(f, "NumberPrefix", b[5] & 63u);
        if (b[13] >= 0xf0 && b[13] <= 0xf9) {
            if (const auto s = bcd(&b[14], 1)) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "%u%02u", b[13] & 15u, *s);
                add(f, "VehicleSeries", std::string(buf));
            }
        }
        optional(f, "VehicleNumber", bcd(&b[15], 2));
        add(f, "StationCode", be(&b[18]));
        optional(f, "RadioNumber", bcd(&b[22], 2));
        char buf[8];
        std::snprintf(buf, sizeof buf, "%02X", b[24]);
        add(f, "UnassignedBytes", hex(b, 20, 22) + " / " + buf);
        kind = "Registration";
        id = hex(b, 9, 12);
        date = stamp(&b[25], 6);
    } else if (n >= 23 && b[0] == 0x15 && b[6] == 0x10 && b[7] == 4) {
        if (sum(&b[15], n - 15) != be(&b[13])) throw DataError("Transfer checksum mismatch");
        identity(f, &b[3]);
        add(f, "ToAll", b[2] == 4 ? 1u : 0u);
        add(f, "Serial", be(&b[8]));
        add(f, "RecordKind", hex(b, 10, 11));
        add(f, "DataKind", hex(b, 21, 22));
        add(f, "UnassignedBytes", hex(b, 11, 13) + " / " + hex(b, 22, n));
        kind = "Transfer";
        id = hex(b, 8, 10);
        date = stamp(&b[15], 6);
    } else if ((b[0] == 1 || b[0] == 3) && n >= 126 && !(b[10] == 1 && b[11] == 1) && (b[10] == 0x10 || b[10] == 0x12)) {
        if (sum(&b[34], n - 2 - 34) != be(&b[n - 2])) throw DataError("Notice checksum mismatch");
        if (b[2] == 0) identity(f, &b[3]);
        const std::pair<const char*, std::pair<std::size_t, std::size_t>> ranges[] = {
            {"TargetMode", {2, 3}}, {"Layout", {10, 11}}, {"Category", {14, 18}}, {"NoticeNumber", {18, 20}},
            {"IssuedAtBytes", {41, 47}}, {"FromStationCode", {105, 108}}, {"ToStationCode", {116, 119}}};
        for (const auto& [name, r] : ranges) add(f, name, hex(b, r.first, r.second));
        optional(f, "TrainNumber", train(&b[29]));
        add(f, "From", notice_text(&b[97], 8, true));
        add(f, "To", notice_text(&b[108], 8, true));
        add(f, "NoticeText", notice_text(&b[124], n - 2 - 124, false));
        add(f, "TextBytes", hex(b, 124, n - 2));
        kind = "Notice";
        id = hex(b, 8, 10);
    } else {
        return unknown(p);
    }
    Message m = message(kind, id, p);
    m.timestamp = date;
    m.fields = std::move(f);
    return m;
}

Message call(const std::array<uint8_t, 28>& info, uint64_t offset) {
    std::vector<Field> f;
    const std::string id = hex(info, 1, 3);
    add(f, "CallAddress", id);
    add(f, "NumberState", hex(info, 3, 4));
    optional(f, "TrainNumber", train(&info[4]));
    add(f, "CallCabCode", hex(info, 7, 8));
    optional(f, "RadioNumber", bcd(&info[10], 2));
    add(f, "CallResultCode", hex(info, 12, 14));
    if (info[12] == 0xff) add(f, "CallSetupStep", info[13]);
    Message m;
    m.kind = "CallSetup";
    m.identity = id;
    m.fields = std::move(f);
    m.raw_hex = hex(info, 0, 28);
    m.first_bit = m.last_bit = offset;
    return m;
}

std::vector<Message> channels(const std::array<uint8_t, 28>& info, uint64_t offset) {
    std::vector<Message> out;
    for (std::size_t i = 0; i < 2; ++i) {
        const uint8_t* r = &info[4 + i * 11];
        std::vector<Field> f;
        const auto ch = bcd(r, 1);
        optional(f, "Channel", ch);
        if (ch) add(f, "Frequency", 352531250u + 6250u * *ch);
        add(f, "ChannelStatus", hex_upper(r + 1, 1));
        add(f, "Serial", r[2]);
        add(f, "NumberState", hex_upper(r + 3, 1));
        if (r[3] != 0) optional(f, "TrainNumber", train(r + 4));
        add(f, "CabRole", hex_upper(r + 7, 1));
        if (r[10] == 0x80) add(f, "RadioNumber", static_cast<uint64_t>(((r[8] & 31u) << 8) | r[9]));
        add(f, "ControlIdentifier", hex(info, 2, 4));
        Message m;
        m.kind = "ChannelActivity";
        m.identity = std::to_string(i);
        m.fields = std::move(f);
        m.raw_hex = hex_upper(r, 11);
        m.first_bit = m.last_bit = offset;
        out.push_back(std::move(m));
    }
    return out;
}

bool slotted_traffic(uint8_t s) { return (s >= 3 && s <= 7) || (s >= 11 && s <= 15); }

std::optional<uint8_t> value7(uint8_t byte) {
    if (byte & 0x80) return static_cast<uint8_t>(byte & 0x7f);
    return std::nullopt;
}
std::optional<std::string> operation_train(const uint8_t* bytes) {
    std::string digits = hex_upper(bytes, 2);
    digits.erase(0, digits.find_first_not_of('F') == std::string::npos ? digits.size() : digits.find_first_not_of('F'));
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }) || bytes[2] < 1 || bytes[2] > 26) return std::nullopt;
    return std::to_string(std::stoi(digits)) + static_cast<char>('A' + bytes[2] - 1);
}

// ---- JSON ----
std::string opt_str(const std::optional<std::string>& s) { return s ? json_string(*s) : "null"; }
std::string opt_num(const std::optional<uint8_t>& v) { return v ? std::to_string(*v) : "null"; }

struct Obj {   // キーの順を指定して JSON オブジェクトを作る
    std::vector<std::pair<std::string, std::string>> kv;
    Obj& add(std::string k, std::string v) {
        kv.emplace_back(std::move(k), std::move(v));
        return *this;
    }
    std::string str(bool sorted) const {
        auto items = kv;
        if (sorted) std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string out = "{";
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out += ',';
            out += json_string(items[i].first) + ":" + items[i].second;
        }
        return out + "}";
    }
};

std::string fields_json(const std::vector<Field>& fields, bool sorted) {
    std::string out = "[";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) out += ',';
        out += Obj{}.add("field", json_string(fields[i].field)).add("value", json_string(fields[i].value)).add("english", opt_str(fields[i].english)).str(sorted);
    }
    return out + "]";
}

std::string summary_json(const OperationSummary& s, bool sorted) {
    std::string rows = "[";
    for (std::size_t i = 0; i < s.rows.size(); ++i) {
        const auto& r = s.rows[i];
        if (i) rows += ',';
        std::string classes = "[" + opt_num(r.delay_classes[0]) + "," + opt_num(r.delay_classes[1]) + "," + opt_num(r.delay_classes[2]) + "]";
        rows += Obj{}
                    .add("line", json_string(r.line))
                    .add("delayed", opt_num(r.delayed))
                    .add("on_line", opt_num(r.on_line))
                    .add("delay_classes", classes)
                    .add("max_delay_min", opt_num(r.max_delay_min))
                    .add("train_no", opt_str(r.train_no))
                    .add("raw_hex", json_string(r.raw_hex))
                    .str(sorted);
    }
    rows += "]";
    return Obj{}
        .add("rows", rows)
        .add("prefix_hex", json_string(s.prefix_hex))
        .add("expanded_hex", json_string(s.expanded_hex))
        .add("prefix_sum_matches", s.prefix_sum_matches ? "true" : "false")
        .str(sorted);
}

std::string message_json(const Message& m, bool sorted) {
    Obj o;
    o.add("kind", json_string(m.kind))
        .add("identity", json_string(m.identity))
        .add("timestamp", opt_str(m.timestamp))
        .add("fields", fields_json(m.fields, sorted))
        .add("raw_hex", json_string(m.raw_hex))
        .add("first_bit", std::to_string(m.first_bit))
        .add("last_bit", std::to_string(m.last_bit));
    if (m.operation_summary) o.add("operation_summary", summary_json(*m.operation_summary, sorted));
    return o.str(sorted);
}

} // namespace

std::string hex_upper(const uint8_t* b, std::size_t n) {
    static const char* d = "0123456789ABCDEF";
    std::string s(2 * n, '0');
    for (std::size_t i = 0; i < n; ++i) {
        s[2 * i] = d[b[i] >> 4];
        s[2 * i + 1] = d[b[i] & 15];
    }
    return s;
}

Format format_for_frequency(double hz) { return std::abs(hz - 352'625'000.0) < 3125.0 ? Format::Common : Format::Slotted; }
const char* to_string(Format f) { return f == Format::Common ? "common" : "slotted"; }

std::optional<std::pair<std::array<uint8_t, 28>, std::size_t>> decode_data_frame(const std::array<uint8_t, 48>& envelope) {
    auto word = descramble(envelope);
    const auto corrections = Galois::get().correct(word, 12);
    if (!corrections) return std::nullopt;
    if (crc16(std::span<const uint8_t>(word.data(), 28)) != be(&word[28])) return std::nullopt;
    std::array<uint8_t, 28> info;
    std::copy_n(word.begin(), 28, info.begin());
    return std::make_pair(info, *corrections);
}

// ---- パケット組立(原本 packet.rs)----

void PacketAssembler::reset() {
    pending_.clear();
    remaining_ = 0;
}

std::optional<Packet> PacketAssembler::accept(const std::array<uint8_t, 28>& info, uint64_t offset) {
    const uint8_t slot = info[0] >> 4;
    if (format_ == Format::Slotted && !slotted_traffic(slot)) return std::nullopt;
    const std::size_t h = format_ == Format::Common ? 0 : 3;
    const uint8_t kind = info[h] & 0xc0, value = info[h] & 63;
    const uint8_t* payload = info.data() + h + 1;
    const std::size_t payload_len = 28 - (h + 1);
    uint8_t distance = 1;
    if (format_ == Format::Slotted)
        while (!slotted_traffic(static_cast<uint8_t>((last_slot_ + distance) % 16))) ++distance;
    const bool consecutive = remaining_ > 0 && offset >= last_ && offset - last_ == 384ull * distance &&
                             (format_ == Format::Common || slot == (last_slot_ + distance) % 16);
    if (kind == 0x80) {
        reset();
        if (value < 1 || static_cast<std::size_t>(value) * payload_len > 257) return std::nullopt;
        first_ = offset;
        remaining_ = value;
    } else if (kind == 0) {
        if (!consecutive || value < 1 || value != remaining_ - 1) {
            reset();
            return std::nullopt;
        }
        remaining_ = value;
    } else {
        const bool finish = kind == 0xc0 || (consecutive && remaining_ == 1);
        const uint64_t first = kind == 0xc0 ? offset : first_;
        std::vector<uint8_t> bytes = kind == 0xc0 ? std::vector<uint8_t>{} : pending_;
        reset();
        const std::size_t n = value;
        if (!finish || n > payload_len || bytes.size() + n > 257 || std::any_of(payload + n, payload + payload_len, [](uint8_t b) { return b != 0; })) return std::nullopt;
        bytes.insert(bytes.end(), payload, payload + n);
        if (bytes.size() < 2 || static_cast<std::size_t>(bytes[1]) + 2 != bytes.size()) return std::nullopt;
        return Packet{std::move(bytes), first, offset};
    }
    if (pending_.size() + payload_len > 257) {
        reset();
        return std::nullopt;
    }
    pending_.insert(pending_.end(), payload, payload + payload_len);
    last_ = offset;
    last_slot_ = slot;
    return std::nullopt;
}

// ---- 運行把握(原本 operation.rs)----

OperationSummary decode_operation(const uint8_t* body, std::size_t n) {
    if (n < 11) throw DataError("Truncated operation table prefix");
    const auto expanded = lha_expand(body + 11, n - 11);
    if (expanded.size() != 1 + 7 * 21 || expanded[0] != 0x87) throw DataError("Unsupported operation table layout");
    OperationSummary s;
    for (std::size_t k = 0; k < 7; ++k) {
        const uint8_t* row = expanded.data() + 1 + 21 * k;
        if (row[0] != 7 || row[1] != 0) throw DataError("Unsupported operation row");
        std::string line;
        for (std::size_t j = 2; j < 10; j += 2) {
            if (!(row[j] >= 0x21 && row[j] <= 0x7e && row[j + 1] >= 0x21 && row[j + 1] <= 0x7e)) throw DataError("Invalid operation line name");
            line += jis(row[j], row[j + 1]);
        }
        for (std::size_t pos; (pos = line.find("\xe3\x80\x80")) != std::string::npos;) line.erase(pos, 3);
        if (line.empty()) throw DataError("Empty operation line name");
        OperationRow r;
        r.line = line;
        r.max_delay_min = value7(row[11]);
        r.train_no = operation_train(row + 13);
        r.delayed = value7(row[16]);
        r.on_line = value7(row[17]);
        r.delay_classes = {value7(row[18]), value7(row[19]), value7(row[20])};
        r.raw_hex = hex_upper(row, 21);
        s.rows.push_back(std::move(r));
    }
    s.prefix_hex = hex_upper(body, 11);
    s.expanded_hex = hex_upper(expanded.data(), expanded.size());
    s.prefix_sum_matches = sum(expanded.data(), expanded.size()) == be(body + 9);
    return s;
}

std::optional<Message> OperationAssembler::accept(const Packet& p) {
    const auto& b = p.bytes;
    if (b.size() < 16 || b[2] != 5 || b[10] != 1 || b[11] != 1 || b[13] != 0xc9 || b[14] != 1 || b[15] != 0x21) {
        *this = OperationAssembler{};
        return unknown(p);
    }
    const uint8_t part = b[12] >> 4, total = b[12] & 15;
    if (part == 0 || part > total) {
        *this = OperationAssembler{};
        throw DataError("Invalid operation table part");
    }
    std::vector<uint8_t> key(b.begin() + 2, b.begin() + 12);
    key.insert(key.end(), {total, b[13], b[14], b[15]});
    if (part == 1) {
        body_.clear();
        key_ = key;
        first_ = p.first;
    } else if (part_ + 1 != part || key_ != key || p.first < last_ || p.first - last_ != 384) {
        *this = OperationAssembler{};
        return std::nullopt;
    }
    body_.insert(body_.end(), b.begin() + 16, b.end());
    part_ = part;
    last_ = p.last;
    if (body_.size() > 15 * 241) {
        *this = OperationAssembler{};
        throw DataError("Operation table size limit");
    }
    if (part != total) return std::nullopt;
    const uint64_t first = first_;
    const std::vector<uint8_t> body = std::move(body_);
    *this = OperationAssembler{};
    auto summary = decode_operation(body.data(), body.size());
    Message m = message("OperationSummary", hex(b, 8, 10), p);
    m.first_bit = first;
    m.raw_hex = hex_upper(body.data(), body.size());
    add(m.fields, "LineCount", summary.rows.size());
    m.operation_summary = std::move(summary);
    return m;
}

void OperationReception::update(std::optional<OperationReception>& current, const Message& message, const std::string& received_at) {
    if (!message.operation_summary) return;
    if (current && current->summary == *message.operation_summary) {
        current->last_received_at = received_at;
        if (current->receptions != UINT64_MAX) ++current->receptions;
        return;
    }
    current = OperationReception{received_at, received_at, 1, *message.operation_summary};
}

// ---- 復号器(原本 mod.rs)----

DataDecoder::DataDecoder(Format f) : format_(f), packets_(f) {}

void DataDecoder::reset_assembly() {
    packets_.reset();
    key_.reset();
    part_ = 0;
    body_.clear();
    operation_ = OperationAssembler{};
}

std::vector<Message> DataDecoder::decode(const std::array<uint8_t, 48>& envelope, uint64_t bit_offset) {
    const auto decoded = decode_data_frame(envelope);
    if (!decoded) {
        ++stats.rejected_frames;
        reset_assembly();
        return {};
    }
    const auto& [info, corrected] = *decoded;
    ++stats.valid_frames;
    if (corrected > 0) ++stats.corrected_frames;
    if (format_ == Format::Slotted) {
        if (((info[0] >> 4) == 0 || (info[0] >> 4) == 8) && info[1] == 0x20) return channels(info, bit_offset);
        if ((info[0] >> 4) == 10 && info[8] == 0x20 && info[9] == 3) return {call(info, bit_offset)};
    }
    const auto packet = packets_.accept(info, bit_offset);
    if (!packet) return {};
    try {
        std::optional<Message> m = format_ == Format::Common ? common(*packet) : std::optional<Message>(traffic(*packet));
        if (m) return {std::move(*m)};
        return {};
    } catch (const DataError&) {
        ++stats.rejected_messages;
        reset_assembly();
        return {};
    }
}

std::optional<Message> DataDecoder::common(const Packet& p) {
    if (!p.bytes.empty() && p.bytes[0] == 0x22) return operation_.accept(p);
    const auto& b = p.bytes;
    const std::size_t n = b.size();
    if (n < 16 || b[0] != 0x21) return unknown(p);
    if (b[2] == 4) {
        if (n < 21 || (n - 21) % 2 != 0 || static_cast<std::size_t>(b[18]) != (n - 21) / 2 || sum(&b[13], n - 2 - 13) != be(&b[n - 2])) throw DataError("Invalid message directory");
        Message m = message("MessageDirectory", "directory", p);
        // BCD 5 項目(YY MM DD hh mm)の後は ID の数(2 進)。秒ではない
        m.timestamp = stamp(&b[13], 5);
        add(m.fields, "MessageIds", pairs(&b[19], n - 2 - 19));
        return m;
    }
    if (b[2] != 5 || b[10] != 1 || b[11] != 1 || !(b[13] == 0x46 || b[13] == 0xc6) || b[14] != 1 || b[15] != 0x21) return unknown(p);
    const uint8_t part = b[12] >> 4, total = b[12] & 15;
    const std::string key = hex(b, 2, 12) + std::to_string(total) + hex(b, 13, 16);
    if (part < 1 || part > total) throw DataError("Invalid service part");
    if (part == 1) {
        body_.clear();
        key_ = key;
        part_ = 0;
        first_ = p.first;
    } else if (key_ != key || part != part_ + 1 || p.first < last_ || p.first - last_ != 384) {
        key_.reset();
        body_.clear();
        return std::nullopt;
    }
    body_.insert(body_.end(), b.begin() + 16, b.end());
    part_ = part;
    last_ = p.last;
    if (body_.size() > 15 * 241) throw DataError("Service size limit");
    if (part != total) return std::nullopt;
    const std::vector<uint8_t> body = std::move(body_);
    body_.clear();
    key_.reset();
    if (body.size() < 10 || body[0] != 1 || body[5] == 0) throw DataError("Invalid service body");
    const std::size_t terminal = 6 + 2 * (static_cast<std::size_t>(body[5]) - 1);
    if (terminal + 4 > body.size() || body[terminal] != 0x99 || body[terminal + 1] != 0x99) throw DataError("Invalid destinations");
    const std::vector<uint8_t> content = b[13] == 0xc6 ? lha_expand(body.data() + terminal + 4, body.size() - terminal - 4)
                                                       : std::vector<uint8_t>(body.begin() + static_cast<std::ptrdiff_t>(terminal) + 4, body.end());
    if (static_cast<uint16_t>(sum(body.data(), terminal + 2) + sum(content.data(), content.size())) != be(&body[terminal + 2])) throw DataError("Service checksum mismatch");
    if (content.size() < 5) throw DataError("Missing service date");
    const std::string date = stamp(content.data(), 5);
    auto fields = service_text(content.data(), content.size());
    add(fields, "RelatedMessage", hex(body, 3, 5));
    add(fields, "DestinationCodes", pairs(&body[6], terminal - 6));
    Message m;
    m.kind = "ServiceInformation";
    m.identity = hex(body, 1, 3);
    m.timestamp = date;
    m.fields = std::move(fields);
    m.raw_hex = hex_upper(content.data(), content.size());
    m.first_bit = first_;
    m.last_bit = p.last;
    return m;
}

// ---- JSON ----

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

std::string to_json(const Message& m) { return message_json(m, false); }
std::string to_json(const OperationSummary& s) { return summary_json(s, false); }
std::string to_json(const std::vector<Field>& f) { return fields_json(f, false); }
std::string to_json_sorted(const Message& m) { return message_json(m, true); }

} // namespace spear::jr_train::protocol
