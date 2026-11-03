// 聞きやすさ優先の音声復号(mrl-celp-c の mrl_decode_enhanced、ビット一致ではない)を Voice の約束(index 付き 320 sample)に合わせる。
//   mrl_decode_enhanced はバスポストフィルタの先読みで 160 sample 遅れる: フレーム n の出力 = [n − 1 の後半 | n の前半]。
//   ここでは n + 1 を復号した時点で、フレーム n の 320 sample(n の前半 + n の後半)を index n で出す(遅れは 1 フレーム = 40 ms)。
//   区間の最後は mrl_enhanced_flush で後半を出す。
// 欠落と index の飛びは jr-voice と同じ規則: 受理したフレームの index だけを出し、そこで区間を閉じて復号器を作り直す。
//   区間の終わりは 10 ms で絞り、区間の始まりは 10 ms で上げる(作り直した復号器の立ち上がりと途切れの雑音を抑える)。
#pragma once

#include "voice.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

struct mrl_enhanced;

namespace spear::jr_train::voice {

class EnhanceStream {
public:
    EnhanceStream();
    ~EnhanceStream();
    EnhanceStream(EnhanceStream&&) noexcept;
    EnhanceStream& operator=(EnhanceStream&&) noexcept;

    bool has_output() const { return output_count_ != 0; }
    std::optional<Output> read();
    void push(uint32_t index, const Payload& payload);   // 失敗は Failure
    void missing_frame();
    void flush();

    uint32_t synthesized = 0;
    uint32_t emitted = 0;

private:
    void restart();
    void close_segment();
    void emit(uint32_t index, Pcm pcm);

    std::unique_ptr<double[]> mem_;   // mrl_enhanced_size() byte(8 byte 境界)
    mrl_enhanced* dec_ = nullptr;
    bool fresh_ = true;               // 区間の最初のフレームがまだ(作り直したばかり)
    bool fade_in_ = false;            // 次に出すフレームの頭を上げる
    std::optional<uint32_t> held_index_;   // 前半だけ出ているフレーム
    std::array<int16_t, 160> held_{};
    std::array<Output, 4> output_{};
    std::size_t output_head_ = 0, output_count_ = 0;
};

} // namespace spear::jr_train::voice
