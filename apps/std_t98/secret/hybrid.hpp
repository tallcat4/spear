// 秘話判定モデル ambe2_hybrid(Conv1d×3 + BiLSTM×2 + attention pooling)— ONNX Runtime で推論(要件 §3.4 道 2)。
// 全鍵探索の冒頭「そもそも平文か」判定と、ffnn が通した候補の絞り込みに使う(cracker.py と同じ役割)。
// モデルは apps/std_t98/models/ambe2_hybrid.onnx(export_secret_onnx.py で safetensors から変換、入力 [N, 980] float、動的 N)を埋め込んだもの。
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace spear::std_t98::secret {

class Hybrid {
public:
    Hybrid();
    ~Hybrid();
    Hybrid(const Hybrid&) = delete;
    Hybrid& operator=(const Hybrid&) = delete;
    bool load(std::span<const uint8_t> onnx_bytes, std::string* err);
    bool load_embedded(std::string* err);
    bool loaded() const;
    // x: n × kInputDim の 0/1(float)→ out: n × 2 logits(0 = 平文、1 = 暗号)
    bool logits(const float* x, std::size_t n, float* out, std::string* err = nullptr) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace spear::std_t98::secret
