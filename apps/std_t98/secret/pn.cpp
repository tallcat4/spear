#include "pn.hpp"
#include "ambe/decoder.hpp"

namespace spear::std_t98::secret {

Pn196 pn_sequence(uint16_t key) {
    Pn196 out{};
    unsigned state = key;
    for (int i = 0; i < kPnBits; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<uint8_t>(state & 1u);
        const unsigned fb = (state & 1u) ^ ((state >> 1) & 1u);
        state >>= 1;
        if (fb) state |= 1u << 14;
    }
    return out;
}

Frame49 keystream_thumbdv(const Pn196& pn, int frame) {
    Frame49 ks{};
    for (int i = 0; i < kBitsPerFrame; ++i) ks[static_cast<std::size_t>(i)] = pn[static_cast<std::size_t>(frame * kBitsPerFrame + ambe::kThumbDv[static_cast<std::size_t>(i)])];
    return ks;
}

Frame49 unpack_frame49(const std::array<uint8_t, 7>& payload) {
    Frame49 b{};
    for (int i = 0; i < kBitsPerFrame; ++i) b[static_cast<std::size_t>(i)] = static_cast<uint8_t>((payload[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1);
    return b;
}

std::array<uint8_t, 7> pack_frame49(const Frame49& bits) {
    std::array<uint8_t, 7> out{};
    for (int i = 0; i < kBitsPerFrame; ++i) out[static_cast<std::size_t>(i / 8)] |= static_cast<uint8_t>((bits[static_cast<std::size_t>(i)] & 1) << (7 - i % 8));
    return out;
}

} // namespace spear::std_t98::secret
