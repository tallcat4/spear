// STD-T98 秘話(音声スクランブル)の PN 系列 — ../std-t98-tools/core/crypto/{pn_sequence,secret_voice}.py の移植。
//
// 鍵 = 15 bit LFSR の初期状態(1..32767)。1 TCH フレーム(AMBE 4 フレーム)につき 196 bit を生成し、
// FEC 後の 49 bit(2450)に XOR する。鍵 0 = 平文(XOR なし)。
// ビット順: Python は ThumbDV 順の bit i に key[frame*49 + THUMBDV_MAP[i]] を掛けるが、THUMBDV_MAP は
// mbelib d 順との対応表そのもの(ambe::kThumbDv)なので、mbelib d 順では d[j] ^= key[frame*49 + j] になる。
#pragma once
#include <array>
#include <cstdint>

namespace spear::std_t98::secret {

inline constexpr int kBitsPerFrame = 49;                          // AMBE 2450 の 1 フレーム
inline constexpr int kFramesPerBurst = 4;                         // 1 TCH フレーム = 4 AMBE フレーム
inline constexpr int kPnBits = kBitsPerFrame * kFramesPerBurst;   // 196
inline constexpr uint16_t kMaxKey = 32767;

using Pn196 = std::array<uint8_t, kPnBits>;
using Frame49 = std::array<uint8_t, kBitsPerFrame>;   // ThumbDV 順(2450 ペイロードのビット順 = 学習モデルの入力順)
using Burst = std::array<Frame49, kFramesPerBurst>;   // 1 TCH フレーム分(スクランブル解除前)

// LFSR: 出力 = state&1、feedback = bit0 ^ bit1、右シフトして bit14 に入れる(generate_pn_sequence_196)
Pn196 pn_sequence(uint16_t key);

// mbelib d 順のフレーム f(0..3)に掛かる 49 bit: pn[f*49 + j]
inline const uint8_t* keystream_d(const Pn196& pn, int frame) { return pn.data() + frame * kBitsPerFrame; }

// ThumbDV 順(モデル入力順)のフレーム f に掛かる 49 bit: pn[f*49 + kThumbDv[i]]
Frame49 keystream_thumbdv(const Pn196& pn, int frame);

// 2450 ペイロード 7 byte(先頭詰め)⇔ ThumbDV 順 49 bit
Frame49 unpack_frame49(const std::array<uint8_t, 7>& payload);
std::array<uint8_t, 7> pack_frame49(const Frame49& bits);

} // namespace spear::std_t98::secret
