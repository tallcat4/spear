// ARIB STD-T98 v2.0, Vol. 1, Table 3-5 (pp. 1-25–1-27).
#pragma once
#include <array>
#include "receiver.hpp"

namespace spear::std_t98 {
inline constexpr int kRegisteredChannels = 97;
// Keep CH1..30 indices and the existing band_center_hz calibration reference.
inline constexpr int kBandReferenceHz = 351293750;
struct RegisteredChannel { int number; bool aerial; int frequency_hz; };
inline constexpr auto kRegisteredPlan = [] {
    std::array<RegisteredChannel, kRegisteredChannels> plan{};
    for (int i = 0; i < 82; ++i)
        plan[i] = {i + 1, false, i < 70 ? 351200000 + i * 6250 : 351031250 + (i - 70) * 6250};
    for (int i = 0; i < 15; ++i)
        plan[82 + i] = {i + 1, true, i < 5 ? 351168750 + i * 6250 : 351106250 + (i - 5) * 6250};
    return plan;
}();
inline ReceiverConfig registered_receiver_config() {
    ReceiverConfig cfg;
    cfg.in_rate = 4e6;
    cfg.pfb_channels = 128; // 800 kHz, decimation by 5
    cfg.num_channels = kRegisteredChannels;
    for (const auto& ch : kRegisteredPlan)
        cfg.channel_bins.push_back((ch.frequency_hz - kBandReferenceHz) / 6250);
    return cfg;
}
} // namespace spear::std_t98
