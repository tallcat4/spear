// AMBE: 実録音の TCH ペイロードに対し、
//   1) FEC の 49 bit が pyambelib(Python、以前の mbelib-neo 経路)と一致すること(ambe_golden.txt)
//   2) PCM が blip25-vocoder 1.0(Rust)とサンプル単位で一致すること(ambe_blip25_golden.txt。会場の録音で誤りと C0 訂正不能を含む)
// golden は音声データを含むのでリポジトリ外(~/spear/golden/std_t98/、無ければ skip)。
// 生成: ambe_golden.txt は std-t98-tools の venv(pyambelib 入り)で、spear-std-t98-decode --payloads の各 9 byte ブロックを
// fec_demod → decode_2450(1 デコーダで連続)し「<block hex> <2450 hex> <pcm×160>」を 1 行ずつ書いたもの。
// ambe_blip25_golden.txt は、rec_20260920_014553 の --payloads をチャネルごとの 9 byte 列にし、blip25c の tools/golden
// (blip25-vocoder 1.0)の dec3600 / fec3600 に通したもの(1 行 = <ch> <block> <ambe_d 7 byte> <c0 c1 c2 c3 の訂正数> <pcm×160>)。
#include "ambe.hpp"
#include "ambe/bits.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
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

namespace {
struct Blip25Case { int channel = 0; std::array<uint8_t, 9> block; std::array<uint8_t, 7> ambe_d; bool c0_lost = false; int errors = 0; std::array<int16_t, 160> pcm; };
std::vector<Blip25Case> load_blip25_golden() {
    std::string path;
    if (const char* d = std::getenv("SPEAR_GOLDEN_DIR")) path = std::string(d) + "/std_t98/ambe_blip25_golden.txt";
    else if (const char* h = std::getenv("HOME")) path = std::string(h) + "/spear/golden/std_t98/ambe_blip25_golden.txt";
    std::ifstream f(path);
    std::vector<Blip25Case> cases;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        Blip25Case c;
        std::string b, d;
        int e[4] = {};
        is >> c.channel >> b >> d >> e[0] >> e[1] >> e[2] >> e[3];
        c.block = from_hex<9>(b);
        c.ambe_d = from_hex<7>(d);
        c.c0_lost = e[0] == 255;
        c.errors = c.c0_lost ? 4 : e[0] + e[1];
        for (auto& v : c.pcm) { int x = 0; is >> x; v = static_cast<int16_t>(x); }
        cases.push_back(c);
    }
    return cases;
}
}

TEST(StdT98Ambe, PcmMatchesBlip25RustAcrossFrames) {
    const auto cases = load_blip25_golden();
    if (cases.empty()) GTEST_SKIP() << "blip25 golden not present";
    std::map<int, std::unique_ptr<AmbeDecoder>> dec;   // Rust 側と同じくチャネルごとに 1 デコーダで連続
    int lost = 0, with_errors = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        auto& d = dec[c.channel];
        if (!d) d = std::make_unique<AmbeDecoder>();
        std::array<int16_t, 160> pcm{};
        const auto r = d->decode_3600(c.block, pcm);
        // 49 bit(ambe_d 順で 7 byte に詰めたもの)と訂正数
        const auto bits = ambe::unpack_thumbdv(r.raw2450);
        std::array<uint8_t, 7> packed{};
        for (std::size_t k = 0; k < 49; ++k) packed[k / 8] |= static_cast<uint8_t>(bits[k] << (7 - k % 8));
        EXPECT_EQ(packed, c.ambe_d) << "case " << i;
        EXPECT_EQ(r.total_errors, c.errors) << "case " << i;
        ASSERT_EQ(pcm, c.pcm) << "case " << i;
        lost += c.c0_lost;
        with_errors += c.errors > 0;
    }
    EXPECT_GT(lost, 0);          // 消失(C0 訂正不能)の経路も通っている
    EXPECT_GT(with_errors, 100);
    std::printf("[ pcm ] %zu frames identical to blip25-vocoder (Rust), %d with corrected errors, %d lost\n", cases.size(), with_errors, lost);
}

TEST(StdT98Ambe, Decode3600EqualsTwoStepWhenErrorFree) {
    // decode_3600 は FEC の訂正数を誤りの程度として合成に渡す。誤りゼロのブロックが続く限り 2450 経路と一致する。
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
