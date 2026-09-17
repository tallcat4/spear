#include "ambe.hpp"
#include "ambe/decoder.hpp"

namespace spear::std_t98 {

std::array<uint8_t, 7> fec_demod_3600_to_2450(std::span<const uint8_t, 9> block, int* total_errors) {
    ambe::Frame4x24 fr;
    ambe::deinterleave_3600(block, fr);
    const auto r = ambe::fec_decode(fr);
    if (total_errors) *total_errors = r.total_errors;
    return ambe::pack_thumbdv(r.d);
}

struct AmbeDecoder::Impl { ambe::Decoder dec; };

AmbeDecoder::AmbeDecoder() : impl_(std::make_unique<Impl>()) {}
AmbeDecoder::~AmbeDecoder() = default;

namespace {
void to_pcm(std::span<const float, 160> buf, std::span<int16_t, 160> pcm) {
    for (std::size_t i = 0; i < 160; ++i) {
        float v = buf[i];
        if (v > 32760.f) v = 32760.f; else if (v < -32760.f) v = -32760.f;
        pcm[i] = static_cast<int16_t>(v);
    }
}
}

void AmbeDecoder::decode_2450(std::span<const uint8_t, 7> payload, std::span<int16_t, 160> pcm) {
    std::array<float, 160> buf{};
    impl_->dec.process(ambe::unpack_thumbdv(payload), 0, 0, false, buf);
    to_pcm(buf, pcm);
}

void AmbeDecoder::decode_3600(std::span<const uint8_t, 9> block, std::span<int16_t, 160> pcm) {
    ambe::Frame4x24 fr;
    ambe::deinterleave_3600(block, fr);
    const auto r = ambe::fec_decode(fr);
    std::array<float, 160> buf{};
    impl_->dec.process(r.d, r.c0_errors, r.total_errors, true, buf);
    to_pcm(buf, pcm);
}

} // namespace spear::std_t98
