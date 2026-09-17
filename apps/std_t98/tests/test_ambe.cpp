// AMBE: 実録音の TCH ペイロードに対し、pyambelib(Python)と C++ 実装の fec_demod / PCM がビット一致すること。
// golden は音声データを含むのでリポジトリ外(~/spear/golden/std_t98/ambe_golden.txt、無ければ skip)。
// 生成: std-t98-tools の venv(pyambelib 入り)で、spear-std-t98-decode --payloads の各 9 byte ブロックを
// fec_demod → decode_2450(1 デコーダで連続)し「<block hex> <2450 hex> <pcm×160>」を 1 行ずつ書く。
#include "ambe.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace spear::std_t98;

namespace {
struct AmbeCase { std::array<uint8_t, 9> block; std::array<uint8_t, 7> p2450; std::array<int16_t, 160> pcm; };

template <std::size_t N> std::array<uint8_t, N> from_hex(const std::string& h) {
    std::array<uint8_t, N> a{};
    for (std::size_t i = 0; i < N && 2 * i + 1 < h.size(); ++i) a[i] = static_cast<uint8_t>(std::stoul(h.substr(2 * i, 2), nullptr, 16));
    return a;
}

std::vector<AmbeCase> load_golden() {
    std::string path;
    if (const char* d = std::getenv("SPEAR_GOLDEN_DIR")) path = std::string(d) + "/std_t98/ambe_golden.txt";
    else if (const char* h = std::getenv("HOME")) path = std::string(h) + "/spear/golden/std_t98/ambe_golden.txt";
    std::ifstream f(path);
    std::vector<AmbeCase> cases;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string b, p;
        is >> b >> p;
        AmbeCase c{from_hex<9>(b), from_hex<7>(p), {}};
        for (auto& v : c.pcm) { int x = 0; is >> x; v = static_cast<int16_t>(x); }
        cases.push_back(c);
    }
    return cases;
}
}

TEST(StdT98Ambe, FecDemodMatchesPyambelib) {
    const auto cases = load_golden();
    if (cases.empty()) GTEST_SKIP() << "ambe golden not present";
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        EXPECT_EQ(fec_demod_3600_to_2450(c.block), c.p2450) << "case " << i;
    }
}

TEST(StdT98Ambe, PcmMatchesPyambelibAcrossFrames) {
    // pyambelib は -O3 -march=native -flto(FMA/ベクトル化)でビルドされており float の演算順が違うため、
    // int16 への切り捨てで ±1 LSB の差が出る。一致率 ≥ 97%(実測 98.1%)、最大差 ≤ 2 LSB(実測 1)を要求する。
    const auto cases = load_golden();
    if (cases.empty()) GTEST_SKIP() << "ambe golden not present";
    AmbeDecoder dec;   // Python 側と同じく 1 デコーダで全ブロック連続(前フレーム状態を引き継ぐ)
    std::size_t total = 0, exact = 0;
    int max_diff = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        std::array<int16_t, 160> pcm{};
        dec.decode_2450(c.p2450, pcm);
        for (std::size_t k = 0; k < 160; ++k) {
            const int d = std::abs(static_cast<int>(pcm[k]) - static_cast<int>(c.pcm[k]));
            ++total; if (d == 0) ++exact; max_diff = std::max(max_diff, d);
            EXPECT_LE(d, 2) << "case " << i << " sample " << k;
        }
    }
    EXPECT_GE(static_cast<double>(exact) / static_cast<double>(total), 0.97) << "exact " << exact << "/" << total << " max diff " << max_diff;
    std::printf("[ pcm ] exact %zu/%zu, max diff %d LSB\n", exact, total, max_diff);
}

TEST(StdT98Ambe, Decode3600EqualsTwoStepWhenErrorFree) {
    // decode_3600 は FEC の誤り数をリピート判定に使う。誤りゼロのブロックが続く限り 2450 経路と一致する。
    const auto cases = load_golden();
    if (cases.empty()) GTEST_SKIP() << "ambe golden not present";
    AmbeDecoder a, b;
    int compared = 0;
    for (const auto& c : cases) {
        int errs = 0;
        const auto p = fec_demod_3600_to_2450(c.block, &errs);
        if (errs != 0) break;
        std::array<int16_t, 160> x{}, y{};
        a.decode_3600(c.block, x);
        b.decode_2450(p, y);
        EXPECT_EQ(x, y);
        ++compared;
    }
    EXPECT_GT(compared, 0);
}
