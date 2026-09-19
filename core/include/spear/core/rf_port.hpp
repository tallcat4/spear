// S.P.E.A.R. core — RF ポート(装置パネルの端子)と UHD の frontend / antenna の対応(純粋、UHD 非依存)
//
// B210(LibreSDR 互換機も同じ)には RX に使える端子が 4 つある: TRXA / RXA / RXB / TRXB。
// UHD ではこれを「frontend(subdev spec `A:A` / `A:B`)× antenna(`TX/RX` / `RX2`)」の 2 段で指定する。
// Spear の外(RfConfig / config() / GUI / 保存 / SigMF)は装置パネルの名前 1 つで扱い、UHD への分解は Radio だけが
// この表を使って行う。対応表はここが唯一の真値(要件 §8.1 の宣言項目 `antenna`)。
// 選択した端子の LED は App 運転中(rx streamer が生きている間)に点灯するので、対応は実機で目視確認できる。
#pragma once

#include <array>
#include <string_view>

namespace spear {

enum class RfPort : unsigned char { TrxA = 0, RxA, RxB, TrxB };

struct RfPortInfo {
    RfPort           port;
    std::string_view name;          // 装置パネルの名前(表示・保存・CLI・SigMF)
    std::string_view frontend;      // UHD dboard frontend 名("A" / "B")— tree /mboards/0/dboards/A/rx_frontends の要素
    std::string_view subdev;        // UHD subdev spec(channel 0 をこの frontend に割り当てる)
    std::string_view uhd_antenna;   // UHD set_rx_antenna の名前
};

inline constexpr std::array<RfPortInfo, 4> kRfPorts{{
    {RfPort::TrxA, "TRXA", "A", "A:A", "TX/RX"},
    {RfPort::RxA,  "RXA",  "A", "A:A", "RX2"},
    {RfPort::RxB,  "RXB",  "B", "A:B", "RX2"},
    {RfPort::TrxB, "TRXB", "B", "A:B", "TX/RX"},
}};

constexpr const RfPortInfo& rf_port_info(RfPort p) { return kRfPorts[static_cast<unsigned>(p)]; }
constexpr std::string_view rf_port_name(RfPort p) { return rf_port_info(p).name; }
constexpr const std::array<RfPortInfo, 4>& all_rf_ports() { return kRfPorts; }

// 名前 → ポート。パネル名(TRXA 等)のほか、互換として UHD の antenna 名(RX2 → RXA、TX/RX → TRXA、frontend A)も受ける。
// 未知の名前は false(黙って既定にしない)。
constexpr bool parse_rf_port(std::string_view s, RfPort* out) {
    for (const auto& i : kRfPorts)
        if (s == i.name) { if (out) *out = i.port; return true; }
    if (s == "RX2")   { if (out) *out = RfPort::RxA;  return true; }
    if (s == "TX/RX") { if (out) *out = RfPort::TrxA; return true; }
    return false;
}

} // namespace spear
