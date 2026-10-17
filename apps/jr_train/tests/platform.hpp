// 参照ベクトルを作った環境の判定(voice/tests/test_voice.cpp と同じ規則)。
// 浮動小数の結果は libm(sin / cos / atan2 / tanh など)に依存するので、ビット一致は参照ベクトルを作った OS・アーキテクチャ
// (refgen が vectors/**/platform に書く。Rust の std::env::consts::{OS, ARCH})でだけ要求する。別の環境では許容差で比べ、
// 差を表示する。その環境でも厳密に比べるには tests/refgen を走らせ直す(docs/apps/jr_train.md「参照ベクトル」)。
#pragma once

#include <fstream>
#include <string>

namespace spear::jr_train::test {

inline std::string this_platform() {
#if defined(__APPLE__)
    std::string os = "macos";
#elif defined(__linux__)
    std::string os = "linux";
#else
    std::string os = "other";
#endif
#if defined(__aarch64__)
    return os + "-aarch64";
#elif defined(__x86_64__)
    return os + "-x86_64";
#else
    return os + "-other";
#endif
}

inline bool same_platform(const std::string& vectors_dir) {
    std::ifstream f(vectors_dir + "/platform");
    std::string p;
    f >> p;
    return p == this_platform();
}

} // namespace spear::jr_train::test
