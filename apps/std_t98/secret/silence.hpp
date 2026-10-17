// Known-plaintext recovery for AMBE-3000R default-mode digital silence.
// Observations: ambe2-c/docs/encoder.md, "無音フレームの仕様".
// This is a measured encoder profile, not a guarantee for every radio/firmware.
#pragma once
#include "pn.hpp"

#include <atomic>
#include <optional>
#include <span>

namespace spear::std_t98::secret {

// Solve 22 known bits over GF(2), then validate all nine AMBE fields against
// the measured silence profile. Input is ThumbDV order; phase is 0..3 in TCH.
// A candidate alone is NOT proof of silence. Engaged value 0 means plaintext.
std::optional<uint16_t> silence_key_candidate(const Frame49& frame, int phase);

// Accept only one key supported by at least two different TCH phases.
// Repeats at the same phase do not provide independent key constraints.
// Ambiguity/no evidence/cancellation -> nullopt; confirmed plaintext -> 0.
std::optional<uint16_t> recover_silence_key(std::span<const Burst> bursts,
                                          const std::atomic<bool>* cancel = nullptr);

} // namespace spear::std_t98::secret
