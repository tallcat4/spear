// S.P.E.A.R. core — 記録フォーマット (要件 §8.4): SigMF 互換 + 独自 sidecar
//
// 二層構成:
//   <name>.sigmf-data   raw samples (ci16_le / cf32_le)
//   <name>.sigmf-meta   SigMF (interop 可能な部分: datatype, sample_rate, captures[frequency, sample_start])
//   <name>.spear.json   sidecar (SigMF で表現できないもの: generation, discontinuity, flags,
//                       tuning history, events, 時刻基準と推定精度 §4.4)
// 外部 JSON ライブラリに依存しない(書き出しは素直に、読み込みは必要キーのみ走査)。
#pragma once

#include "types.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace spear::sigmf {

struct Capture {
    uint64_t sample_start = 0;   // data file 内の sample offset
    double   frequency    = 0.0; // Hz
    uint64_t generation   = 0;   // sidecar 側
    HwTime   hw_time;            // 先頭 sample の hardware time
    uint64_t host_ns      = 0;
    uint64_t sample_index = 0;   // generation 内 index (provenance 逆算用)
};

// sidecar: 欠落・異常の記録。"無言の欠落は存在しない" をファイルにも適用する。
struct Discontinuity {
    uint64_t file_sample = 0;    // data file 内の位置(この直前で切れている)
    uint64_t generation  = 0;
    uint64_t sample_index = 0;   // generation 内 index
    uint64_t missing_samples = 0; // 分かる場合(generation 跨ぎは 0)
    std::string flags;
};

struct EventRecord {
    uint64_t host_ns = 0;
    std::string kind, source, detail;
    SampleRange range;
    int64_t value = 0;
};

// tuning history (§8.4): どの sample から新周波数か (§17.1 timed command で sample 単位に確定)
struct Tuning {
    uint64_t generation = 0;
    uint64_t sample_index = 0;
    double   frequency = 0.0;
    uint64_t host_ns = 0;
};

struct TimeRef {
    uint64_t generation = 0;
    double   hw_time_s = 0;
    uint64_t hw_sample_index = 0;
    uint64_t host_mono_ns = 0;
    uint64_t utc_ns = 0;
};

struct Meta {
    DataType    dtype = DataType::ComplexInt16;
    double      sample_rate = 0.0;
    std::string description;
    std::string hw = "Ettus B210 via UHD";
    std::string author = "spear";
    std::string time_reference = "usrp_time_spec";      // §4.4: 基準種別
    std::string time_accuracy  = "TCXO ~2ppm, no GPSDO"; // §4.4: 推定精度
    double      lo_correction_ppm = 0;   // 録音時に Radio が LO 側で打ち消した個体誤差 [ppm](provenance。0 なら書かない)
    std::string rx_port;                 // 受信に使った装置の端子(TRXA / RXA / RXB / TRXB。provenance。空なら書かない)
    std::vector<Capture>       captures;
    std::vector<Discontinuity> discontinuities;
    std::vector<Tuning>        tuning;
    std::vector<TimeRef>       time_refs;     // §4.4: generation ごとの hw↔host↔UTC 対応点
    std::vector<EventRecord>   events;
    uint64_t total_samples = 0;
};

std::string datatype_string(DataType t); // "ci16_le" 等
DataType    parse_datatype(const std::string& s);

// base_path は拡張子なし。3ファイルを書く。
bool write(const std::string& base_path, const Meta& m, std::string* err = nullptr);
// .sigmf-meta と(あれば).spear.json を読む。
bool read(const std::string& base_path, Meta& m, std::string* err = nullptr);

std::string data_path(const std::string& base);
std::string meta_path(const std::string& base);
std::string sidecar_path(const std::string& base);

} // namespace spear::sigmf
