// S.P.E.A.R. core — 基本型 (要件 §4)
//
// Stream / Event / State の3分類を混同しない (§4.1)。
// この header は依存を持たない。
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <string_view>

namespace spear {

// ---- サンプル型 ------------------------------------------------------------
using sc16 = std::complex<int16_t>;   // B210 native wire format (10 Msps = 40 MB/s, §1.1)
using cf32 = std::complex<float>;

enum class DataType : uint8_t {
    Unknown = 0,
    ComplexInt16,   // sc16
    ComplexFloat32, // cf32
    Float32,
    Int16,
    UInt8,
};

template <class T> constexpr DataType dtype_of();
template <> constexpr DataType dtype_of<sc16>()    { return DataType::ComplexInt16; }
template <> constexpr DataType dtype_of<cf32>()    { return DataType::ComplexFloat32; }
template <> constexpr DataType dtype_of<float>()   { return DataType::Float32; }
template <> constexpr DataType dtype_of<int16_t>() { return DataType::Int16; }
template <> constexpr DataType dtype_of<uint8_t>() { return DataType::UInt8; }

constexpr std::size_t dtype_size(DataType t) {
    switch (t) {
    case DataType::ComplexInt16:   return sizeof(sc16);
    case DataType::ComplexFloat32: return sizeof(cf32);
    case DataType::Float32:        return sizeof(float);
    case DataType::Int16:          return sizeof(int16_t);
    case DataType::UInt8:          return sizeof(uint8_t);
    default:                       return 0;
    }
}
std::string_view to_string(DataType t);

enum class Direction : uint8_t { RX, TX };

// ---- Stream metadata (§4.2) ------------------------------------------------
// stream 生成時に確定し、以後不変。
struct StreamMeta {
    std::string id;                 // 静的な文字列定数 ("radio.rx", "std_t98.discriminator")
    DataType    dtype = DataType::Unknown;
    double      sample_rate = 0.0;  // Hz
    double      center_freq = 0.0;  // Hz (baseband なら 0)
    double      bandwidth = 0.0;    // Hz
    std::string unit;               // physical unit ("V", "rad", "Hz", "dBFS", "")
    Direction   direction = Direction::RX;
};

// ---- Sample Block flags (§4.3) ----------------------------------------------
// UHD の error_code / out_of_sequence をここへ「区別したまま」写す。潰さない (§17.1)。
enum class Flag : uint16_t {
    None          = 0,
    Discontinuity = 1u << 0, // 直前の block との間に欠落がある(どの理由であれ)
    Overflow      = 1u << 1, // host が読み遅れた (ERROR_CODE_OVERFLOW)
    Underflow     = 1u << 2, // TX: host が書き遅れた
    Timeout       = 1u << 3, // recv() timeout (USB は生きているがデータが来ない)
    OutOfSequence = 1u << 4, // transport 層のパケット欠落 (USB 不調はここに出る)
    Retune        = 1u << 5, // この block 内(先頭)で center frequency が変わった
    ClockReset    = 1u << 6, // time_spec 原点リセット。generation も進む
    StartOfBurst  = 1u << 7,
    EndOfBurst    = 1u << 8,
    EndOfStream   = 1u << 9, // stream lifecycle: 以後 block は来ない
};

struct Flags {
    uint16_t bits = 0;
    constexpr Flags() = default;
    constexpr Flags(Flag f) : bits(static_cast<uint16_t>(f)) {}
    constexpr bool has(Flag f) const { return bits & static_cast<uint16_t>(f); }
    constexpr Flags& set(Flag f)   { bits |= static_cast<uint16_t>(f); return *this; }
    constexpr Flags& operator|=(Flags o) { bits |= o.bits; return *this; }
    constexpr bool any() const { return bits != 0; }
};
constexpr Flags operator|(Flags a, Flags b) { Flags r = a; r |= b; return r; }
constexpr Flags operator|(Flag a, Flag b) { return Flags(a) | Flags(b); }
std::string to_string(Flags f);

// ---- 時間 (§4.4) -------------------------------------------------------------
// 一次基準は USRP sample counter (rx_metadata_t::time_spec)。
struct HwTime {
    int64_t full_secs = 0;
    double  frac_secs = 0.0;
    bool    valid = false;
    double  seconds() const { return static_cast<double>(full_secs) + frac_secs; }
};

// ---- Sample Block header (§4.3) ---------------------------------------------
struct BlockHeader {
    uint64_t generation   = 0; // §4.4.1: 異なる generation 間で timestamp/sample_index を比較してはならない
    uint64_t sequence     = 0; // producer が発行した通し番号(block 単位)
    uint64_t sample_index = 0; // generation 内の先頭 sample の index (provenance の土台, §4.5)
    HwTime   hw_time;          // 先頭 sample の hardware timestamp
    uint64_t host_ns      = 0; // host monotonic clock (UTC mapping 用, §4.4)
    uint32_t sample_count = 0;
    DataType dtype        = DataType::Unknown;
    Flags    flags;

    uint64_t sample_end() const { return sample_index + sample_count; } // exclusive
};

// ---- Provenance (§4.5 最小版) -----------------------------------------------
struct SampleRange {
    uint64_t generation = 0;
    uint64_t begin = 0; // inclusive
    uint64_t end   = 0; // exclusive
    bool empty() const { return end <= begin; }
};

// ---- Device state (§12.1, §17.2) --------------------------------------------------
// USRP の主要状態。推定ではなく一次ソース(libusb 列挙 / FX3 状態レジスタ / UHD センサ / stream 状態)
// から判定する。evidence にその根拠を必ず残す。
enum class DeviceState : uint8_t {
    Unknown,       // まだ probe していない
    Disconnected,  // USB 上に B2xx が存在しない(libusb 列挙)
    NoFirmware,    // 存在するが FX3 はブートローダ(UHD firmware 未ロード)。UHD が次の open で書き込む
    Standby,       // FX3 firmware 動作中、FPGA 未構成または構成中(FX3 状態: unconfigured/fpga_ready/configuring/busy)
    Initializing,  // FX3 running。device open / tune / lock 確認の途中(起動シーケンス stage 2〜5)
    Ready,         // FX3 running = FPGA 構成済みで即動作可能(open 前、または open 済み・lock 確認済み・stream 停止中)
    Streaming,     // 定常受信中(stage 6)
    Fault,         // FX3 error / lock timeout / serial・compat 不一致 / 他プロセスが占有 など。detail 参照
    Lost,          // 動作中に device が消失。後始末して再 probe へ
};
std::string_view to_string(DeviceState s);

// 受信中も安全に読めるセンサ群(EP4 制御経路。EP0 の FX3 レジスタと違い転送を止めない — 実測済み)
struct LiveSensors {
    bool   valid = false;
    bool   lo_locked = true;
    bool   ref_locked = false;     // external/gpsdo のときのみ意味を持つ
    double rx_temp_c = 0;          // AD9361 温度
    double rssi_db = 0;
    double hw_time_s = 0;          // get_time_now
    uint64_t host_ns = 0;
    double drift_ppm = 0;          // 時刻基準からの hw clock vs host monotonic の乖離 (§4.4 精度推定)
    double drift_uncertainty_ppm = 0; // 往復遅延由来の不確かさ。|drift| がこれを超えて初めて意味を持つ
    uint64_t sampled_ns = 0;
};

struct DeviceStatus {
    DeviceState state = DeviceState::Unknown;
    std::string evidence;      // 判定根拠(一次ソースの値をそのまま書く)
    std::string serial;        // 見えている個体
    std::string fx3_state;     // FX3 状態レジスタの文字列(取得できたとき)
    int         stage = 0;     // 起動シーケンスの到達段 (0..6)
    uint64_t    since_ns = 0;  // この状態に入った host 時刻
    uint64_t    generation = 0;
    LiveSensors sensors;
};

// 時刻基準 (§4.4): streaming 開始時に hardware time と host clock の対応を 1 点記録する
struct TimeReference {
    uint64_t generation = 0;
    double   hw_time_s = 0;        // USRP get_time_now()(往復の中点で対応付け)
    uint64_t hw_sample_index = 0;  // hw_time_s に対応する sample index (generation 内)。tick 差分から算出
    uint64_t host_mono_ns = 0;     // steady_clock(往復の中点)
    uint64_t utc_ns = 0;           // system_clock (UTC)
    uint64_t round_trip_ns = 0;    // get_time_now の往復時間 = 対応点の不確かさ(±半分)
    std::string reference = "usrp_time_spec";
    std::string accuracy  = "TCXO ~2ppm, no GPSDO";
    bool valid = false;
};

// ---- RF 宣言 (§8.1) -------------------------------------------------------------
// App が Core へ宣言する RF 条件。Core はそのまま適用する(適用不能なら App 起動失敗)。
struct RfConfig {
    Direction   direction   = Direction::RX;
    double      center_freq = 100e6;
    double      sample_rate = 2e6;   // 2〜10 Msps を基本 (§1.1)
    double      bandwidth   = 0.0;   // 0 = sample_rate 準拠
    double      gain        = 30.0;  // dB(agc のときは AGC を切ったときに戻る値)
    bool        agc         = false; // 受信 AGC(AD9361)。true なら gain は装置が決める
    std::string antenna     = "RX2";
    std::string clock_source = "internal";
};

} // namespace spear
