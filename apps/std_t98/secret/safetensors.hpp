// safetensors(https://github.com/huggingface/safetensors)の最小リーダ。
// 形式: u64 LE ヘッダ長 + JSON ヘッダ {"name": {"dtype": "F32", "shape": [...], "data_offsets": [begin, end]}, "__metadata__": {...}} + 生データ。
// F32 テンソルだけを扱う(秘話モデルの重みはすべて F32)。学習フレームワークには依存しない(要件 §3.4 道 1)。
#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace spear::std_t98::secret {

struct Tensor {
    std::vector<int64_t> shape;
    std::vector<float> data;   // row-major
    int64_t numel() const { int64_t n = 1; for (auto d : shape) n *= d; return n; }
};

// 失敗時は false と err。
bool read_safetensors(std::span<const uint8_t> bytes, std::map<std::string, Tensor>& out, std::string* err);

} // namespace spear::std_t98::secret
