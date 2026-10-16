#include "ambe.hpp"
#include "ambe/bits.hpp"

#include "blip25.h"

namespace spear::std_t98 {

namespace {
// 9 byte → 49 bit(ambe_d 順)。戻り値: 訂正数(C0 + C1)、C0 が訂正不能なら −1
int fec(std::span<const uint8_t, 9> block, ambe::Bits49& d) { return blip25_fec_3600(block.data(), d.data(), nullptr); }
}

std::array<uint8_t, 7> fec_demod_3600_to_2450(std::span<const uint8_t, 9> block, int* total_errors) {
    ambe::Bits49 d{};
    const int e = fec(block, d);
    if (total_errors) *total_errors = e < 0 ? 4 : e;
    return ambe::pack_thumbdv(d);
}

struct AmbeDecoder::Impl { blip25_decoder dec; };

AmbeDecoder::AmbeDecoder() : impl_(std::make_unique<Impl>()) { blip25_init(&impl_->dec); }
AmbeDecoder::~AmbeDecoder() = default;

void AmbeDecoder::decode_2450(std::span<const uint8_t, 7> payload, std::span<int16_t, 160> pcm) {
    blip25_decode_2450_thumbdv(&impl_->dec, payload.data(), pcm.data());
}

AmbeDecoder::Decoded3600 AmbeDecoder::decode_3600(std::span<const uint8_t, 9> block, std::span<int16_t, 160> pcm, const uint8_t* keystream) {
    ambe::Bits49 d{};
    const int e = fec(block, d);
    Decoded3600 out{ambe::pack_thumbdv(d), e < 0 ? 4 : e};
    if (keystream) for (std::size_t i = 0; i < d.size(); ++i) d[i] = static_cast<uint8_t>(d[i] ^ keystream[i]);   // 秘話: FEC 後・音声復号前に XOR(secret_voice.py と同じ位置)
    // 誤りの程度 = 訂正数(blip25_decode_3600 と同じ)。C0 が訂正不能なフレームは消失として前フレームの繰り返し / フェードにする
    blip25_decode_bits(&impl_->dec, d.data(), e < 0 ? BLIP25_LOST : e, pcm.data());
    return out;
}

} // namespace spear::std_t98
