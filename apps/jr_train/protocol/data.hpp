// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// データ電文。原本: crates/receiver-core/src/data/(mod.rs / packet.rs / text.rs / lha.rs / operation.rs、commit dfc7440)。
//   データフレーム: デスクランブル → RS(42,30)(12 根、t = 6)→ CRC-16(byte 0–27 → 28–29)→ 情報 28 byte。
//   Slotted(データ波・音声波): 16 スロット周期、トラフィックスロット 3–7 / 11–15。チャネル状況・呼設定は単独フレーム。
//   Common(旅客一斉情報 = 共用波): 全フレームがパケットの断片。0x21 = 一覧 / 旅客案内、0x22 = 運行把握(LH5 圧縮の線区表)。
//   パケットの断片: ヘッダ 0x80 = 先頭(残り数)、0x00 = 続き、0x40 / 0xC0 = 末尾(長さ)。最大 257 byte、bytes[1] + 2 = 長さ。
// 解釈できない電文は種別 "Unsupported" として生のまま残す。電文の JSON は原本(serde)と同じ形・同じキー順。
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace spear::jr_train::protocol {

enum class Format { Common, Slotted };
Format format_for_frequency(double hz);   // 原本 Format::for_frequency(352.625 MHz ± 3.125 kHz なら Common)
const char* to_string(Format f);           // "common" / "slotted"

struct Field {
    std::string field, value;
    std::optional<std::string> english;
};

struct OperationRow {
    std::string line;                       // 線区名(全角空白を除く)
    std::optional<uint8_t> delayed, on_line;
    std::array<std::optional<uint8_t>, 3> delay_classes{};   // 3–9 分 / 10–19 分 / 20 分以上
    std::optional<uint8_t> max_delay_min;
    std::optional<std::string> train_no;
    std::string raw_hex;
    bool operator==(const OperationRow&) const = default;
};

struct OperationSummary {
    std::vector<OperationRow> rows;
    std::string prefix_hex, expanded_hex;
    bool prefix_sum_matches = false;        // 観測された関係だけ(確立した検査ではない)
    bool operator==(const OperationSummary&) const = default;
};

struct Message {
    std::string kind, identity;
    std::optional<std::string> timestamp;   // 電文中の日時(運行把握では常に無し)
    std::vector<Field> fields;
    std::string raw_hex;
    uint64_t first_bit = 0, last_bit = 0;
    std::optional<OperationSummary> operation_summary;
};

// 運行把握の受信状態(原本 OperationReception)。同じ内容の再送は回数を数え、変われば置き換える
struct OperationReception {
    std::string first_received_at, last_received_at;
    uint64_t receptions = 0;
    OperationSummary summary;
    static void update(std::optional<OperationReception>& current, const Message& message, const std::string& received_at);
};

struct DataStats {
    uint64_t valid_frames = 0, corrected_frames = 0, rejected_frames = 0, rejected_messages = 0;
};

// 原本の Err(&'static str)
class DataError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Packet {
    std::vector<uint8_t> bytes;
    uint64_t first = 0, last = 0;
};

// データフレームの復号: 成功なら (情報 28 byte, 訂正シンボル数)
std::optional<std::pair<std::array<uint8_t, 28>, std::size_t>> decode_data_frame(const std::array<uint8_t, 48>& envelope);

class PacketAssembler {
public:
    explicit PacketAssembler(Format f) : format_(f) {}
    void reset();
    std::optional<Packet> accept(const std::array<uint8_t, 28>& info, uint64_t offset);

private:
    Format format_;
    std::vector<uint8_t> pending_;
    uint8_t remaining_ = 0, last_slot_ = 0;
    uint64_t first_ = 0, last_ = 0;
};

class OperationAssembler {
public:
    // 戻り値: 完成した電文(または Unsupported)。DataError は原本の Err
    std::optional<Message> accept(const Packet& p);

private:
    std::vector<uint8_t> key_, body_;
    uint8_t part_ = 0;
    uint64_t first_ = 0, last_ = 0;
};

class DataDecoder {
public:
    explicit DataDecoder(Format f);
    void reset_assembly();
    std::vector<Message> decode(const std::array<uint8_t, 48>& envelope, uint64_t bit_offset);
    DataStats stats;
    Format format() const { return format_; }

private:
    std::optional<Message> common(const Packet& p);
    Format format_;
    PacketAssembler packets_;
    std::vector<uint8_t> body_;
    std::optional<std::string> key_;
    uint8_t part_ = 0;
    uint64_t first_ = 0, last_ = 0;
    OperationAssembler operation_;
};

// ---- 部品(テストからも使う)----
std::string hex_upper(const uint8_t* b, std::size_t n);
std::vector<uint8_t> lha_expand(const uint8_t* a, std::size_t n);         // 失敗は DataError
std::string jis(uint8_t row, uint8_t cell);                               // 失敗は DataError
std::string notice_text(const uint8_t* b, std::size_t n, bool trim);
std::vector<Field> service_text(const uint8_t* b, std::size_t n);         // 失敗は DataError
OperationSummary decode_operation(const uint8_t* body, std::size_t n);    // 失敗は DataError

// ---- JSON(原本の serde 出力と同じ)----
std::string json_string(const std::string& s);
std::string to_json(const Message& m);                // 構造体の順(data.jsonl / messages.json)
std::string to_json(const OperationSummary& s);
std::string to_json(const std::vector<Field>& f);
std::string to_json_sorted(const Message& m);         // serde_json::Value 経由の順(キーを辞書順。保存の JSONL)

} // namespace spear::jr_train::protocol
