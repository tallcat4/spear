#include "enhance.hpp"

#include "common.hpp"
#include "mrlcelp.h"

#include <cmath>
#include <numbers>

namespace spear::jr_train::voice {

namespace {
constexpr std::size_t kFade = 80;   // 10 ms

// 二乗余弦の 0 → 1(n = 0..kFade-1)
float rise(std::size_t n) { return 0.5f - 0.5f * std::cos(std::numbers::pi_v<float> * (static_cast<float>(n) + 0.5f) / static_cast<float>(kFade)); }
int16_t scale(int16_t v, float g) { return static_cast<int16_t>(std::lrint(static_cast<float>(v) * g)); }
} // namespace

EnhanceStream::EnhanceStream() : mem_(std::make_unique<double[]>((mrl_enhanced_size() + sizeof(double) - 1) / sizeof(double))) { restart(); }
EnhanceStream::~EnhanceStream() = default;
EnhanceStream::EnhanceStream(EnhanceStream&&) noexcept = default;
EnhanceStream& EnhanceStream::operator=(EnhanceStream&&) noexcept = default;

void EnhanceStream::restart() {
    dec_ = mrl_enhanced_init(mem_.get(), nullptr);
    if (!dec_) throw Failure{};
    fresh_ = true;
    fade_in_ = true;
    held_index_.reset();
}

std::optional<Output> EnhanceStream::read() {
    if (output_count_ == 0) return std::nullopt;
    const Output r = output_[output_head_];
    output_head_ = (output_head_ + 1) % output_.size();
    --output_count_;
    return r;
}

void EnhanceStream::push(uint32_t index, const Payload& payload) {
    if (held_index_ && static_cast<uint64_t>(index) != static_cast<uint64_t>(*held_index_) + 1) close_segment();
    std::array<int16_t, 320> pcm{};
    if (mrl_decode_enhanced(dec_, payload.data(), pcm.data()) != 0) throw Failure{};
    // 区間の最初の出力の前半は先読みの 0(捨てる)。以降は [前のフレームの後半 | このフレームの前半]
    if (!fresh_ && held_index_) {
        Pcm out{};
        std::copy(held_.begin(), held_.end(), out.begin());
        std::copy(pcm.begin(), pcm.begin() + 160, out.begin() + 160);
        emit(*held_index_, out);
    }
    std::copy(pcm.begin() + 160, pcm.end(), held_.begin());
    held_index_ = index;
    fresh_ = false;
}

void EnhanceStream::missing_frame() { close_segment(); }

void EnhanceStream::flush() { close_segment(); }

void EnhanceStream::close_segment() {
    if (fresh_) return;
    if (held_index_) {
        std::array<int16_t, 160> tail{};
        mrl_enhanced_flush(dec_, tail.data());
        Pcm out{};
        std::copy(held_.begin(), held_.end(), out.begin());
        std::copy(tail.begin(), tail.end(), out.begin() + 160);
        for (std::size_t n = 0; n < kFade; ++n) out[out.size() - 1 - n] = scale(out[out.size() - 1 - n], rise(n));
        emit(*held_index_, out);
    }
    restart();
}

void EnhanceStream::emit(uint32_t index, Pcm pcm) {
    if (output_count_ == output_.size()) throw Failure{};
    if (fade_in_)
        for (std::size_t n = 0; n < kFade; ++n) pcm[n] = scale(pcm[n], rise(n));
    fade_in_ = false;
    output_[(output_head_ + output_count_) % output_.size()] = Output{index, pcm};
    ++output_count_;
    ++synthesized;
    ++emitted;
}

} // namespace spear::jr_train::voice
