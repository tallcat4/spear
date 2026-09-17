// 学習済みモデルをバイナリに埋め込む(apps/std_t98/models/、.incbin)。実行時のファイルパスを持たない(read-only rootfs でも動く、§3.4)。
#pragma once
#include <cstdint>
#include <span>

namespace spear::std_t98::secret {

std::span<const uint8_t> embedded_ffnn_safetensors();
std::span<const uint8_t> embedded_hybrid_onnx();

} // namespace spear::std_t98::secret
