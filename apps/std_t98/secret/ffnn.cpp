#include "ffnn.hpp"
#include "models.hpp"
#include "safetensors.hpp"

#include <map>

namespace spear::std_t98::secret {

bool Ffnn::load_embedded(std::string* err) { return load(embedded_ffnn_safetensors(), err); }

bool Ffnn::load(std::span<const uint8_t> bytes, std::string* err) {
    std::map<std::string, Tensor> t;
    if (!read_safetensors(bytes, t, err)) return false;
    for (const char* k : {"fc1.weight", "fc1.bias", "fc2.weight", "fc2.bias"})
        if (!t.count(k)) { if (err) *err = std::string("ffnn: missing tensor ") + k; return false; }
    const auto& w1 = t["fc1.weight"];
    const auto& w2 = t["fc2.weight"];
    if (w1.shape.size() != 2 || w1.shape[1] != kInputDim || w2.shape.size() != 2 || w2.shape[0] != 2 || w2.shape[1] != w1.shape[0]) {
        if (err) *err = "ffnn: unexpected tensor shapes";
        return false;
    }
    hidden_ = static_cast<int>(w1.shape[0]);
    w1t_.assign(static_cast<std::size_t>(kInputDim) * hidden_, 0.f);
    for (int h = 0; h < hidden_; ++h)
        for (int i = 0; i < kInputDim; ++i) w1t_[static_cast<std::size_t>(i) * hidden_ + h] = w1.data[static_cast<std::size_t>(h) * kInputDim + i];
    b1_ = t["fc1.bias"].data;
    w2_ = w2.data;
    b2_[0] = t["fc2.bias"].data[0];
    b2_[1] = t["fc2.bias"].data[1];
    return true;
}

void Ffnn::logits(const uint8_t* x, float out[2]) const {
    const int H = hidden_;
    float h[512];   // hidden ≤ 512(モデルは 128)
    for (int k = 0; k < H; ++k) h[k] = b1_[static_cast<std::size_t>(k)];
    for (int i = 0; i < kInputDim; ++i) {
        if (!x[i]) continue;
        const float* col = &w1t_[static_cast<std::size_t>(i) * H];
        for (int k = 0; k < H; ++k) h[k] += col[k];
    }
    float a0 = b2_[0], a1 = b2_[1];
    const float* r0 = w2_.data();
    const float* r1 = w2_.data() + H;
    for (int k = 0; k < H; ++k) {
        const float v = h[k] > 0.f ? h[k] : 0.f;
        a0 += v * r0[k];
        a1 += v * r1[k];
    }
    out[0] = a0;
    out[1] = a1;
}

} // namespace spear::std_t98::secret
