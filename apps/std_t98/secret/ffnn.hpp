// 秘話判定モデル ambe2_ffnn(cracker.py AMBE2Classifier: Linear 980→128, ReLU, Linear 128→2)の C++ 推論(要件 §3.4 道 1)。
// 入力 = 20 AMBE フレーム × 49 bit(ThumbDV 順)の 0/1、出力 logits[0] = 平文、[1] = 暗号。重みは埋め込みの safetensors から直接読む。
// 全 32767 鍵のふるい分けに使うので、入力がビットであることを使って fc1 を「立っているビットの列の総和」で計算する。
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spear::std_t98::secret {

inline constexpr int kFramesPerBlock = 20;                                 // 5 バースト × 4 フレーム
inline constexpr int kInputDim = kFramesPerBlock * 49;                     // 980
inline constexpr int kLabelPlain = 0, kLabelEncrypted = 1;

class Ffnn {
public:
    bool load(std::span<const uint8_t> safetensors_bytes, std::string* err);
    bool load_embedded(std::string* err);
    bool loaded() const { return hidden_ > 0; }
    int hidden() const { return hidden_; }
    // x: kInputDim 個の 0/1 → logits[2]
    void logits(const uint8_t* x, float out[2]) const;
    // plain − encrypted(> 0 なら平文と判定)
    float margin(const uint8_t* x) const { float l[2]; logits(x, l); return l[kLabelPlain] - l[kLabelEncrypted]; }
private:
    int hidden_ = 0;
    std::vector<float> w1t_;   // [kInputDim][hidden](列 = 入力ビット 1 本分)
    std::vector<float> b1_;    // [hidden]
    std::vector<float> w2_;    // [2][hidden]
    float b2_[2] = {0, 0};
};

} // namespace spear::std_t98::secret
