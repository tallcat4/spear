// 差分ファジング: tests/refgen(bin fuzz)が乱数の入力を原本の Rust に通した結果と、C++ の結果を比べる。
// 入力が大きい(約 50 MB)のでリポジトリには入れない。環境変数 JR_TRAIN_FUZZ_DIR があるときだけ走る:
//   cd apps/jr_train/tests/refgen && CARGO_TARGET_DIR=/tmp/jr-refgen cargo run --release --bin fuzz -- /tmp/jr-fuzz [規模]
//   JR_TRAIN_FUZZ_DIR=/tmp/jr-fuzz ./spear_jr_train_tests --gtest_filter='JrTrainFuzz*'
// 浮動小数に依存するもの(音声の PCM、DSP の dibit)は、生成した環境(platform)と同じときだけ一致を要求する。
#include "dsp/jr_dsp.hpp"
#include "platform.hpp"
#include "protocol/data.hpp"
#include "protocol/voice_frame.hpp"
#include "voice/voice.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace spear::jr_train;
using namespace spear::jr_train::protocol;

const char* fuzz_dir() { return std::getenv("JR_TRAIN_FUZZ_DIR"); }
#define REQUIRE_FUZZ()                                                                    \
    if (!fuzz_dir()) GTEST_SKIP() << "JR_TRAIN_FUZZ_DIR が無い(tests/refgen の bin fuzz で作る)"

std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> out(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(std::stoi(s.substr(2 * i, 2), nullptr, 16));
    return out;
}
std::string lowhex(const uint8_t* b, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += d[b[i] >> 4];
        s += d[b[i] & 15];
    }
    return s;
}
std::array<uint8_t, 48> env(const std::string& h) {
    const auto v = unhex(h);
    std::array<uint8_t, 48> e{};
    std::memcpy(e.data(), v.data(), 48);
    return e;
}
struct Mismatch {
    std::size_t n = 0;
    void add(const std::string& what) {
        if (++n <= 5) ADD_FAILURE() << what.substr(0, 600);
    }
};

TEST(JrTrainFuzz, VoiceAndDataFrames) {
    REQUIRE_FUZZ();
    std::ifstream f(std::string(fuzz_dir()) + "/frames.txt");
    std::string line;
    std::size_t n = 0, voice_ok = 0, data_ok = 0;
    Mismatch bad;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string e, vp, vc, dw, dc;
        is >> e >> vp >> vc >> dw >> dc;
        const auto envl = env(e);
        const auto v = decode_voice(envl);
        const std::string got_vp = v ? lowhex(v->payload.data(), 20) : "-";
        const std::string got_vc = v ? std::to_string(v->corrected_bits) : "-";
        const auto d = decode_data_frame(envl);
        const std::string got_dw = d ? lowhex(d->first.data(), 28) : "-";
        const std::string got_dc = d ? std::to_string(d->second) : "-";
        if (got_vp != vp || got_vc != vc || got_dw != dw || got_dc != dc)
            bad.add(line + "\n got: " + got_vp + " " + got_vc + " " + got_dw + " " + got_dc);
        voice_ok += vp != "-";
        data_ok += dw != "-";
        ++n;
    }
    std::printf("  frames: %zu cases (voice accepted %zu, data accepted %zu), mismatches %zu\n", n, voice_ok, data_ok, bad.n);
    EXPECT_GT(n, 0u);
    EXPECT_EQ(bad.n, 0u);
}

TEST(JrTrainFuzz, Lh5NoticeAndServiceText) {
    REQUIRE_FUZZ();
    std::ifstream f(std::string(fuzz_dir()) + "/text.txt");
    std::string line;
    std::size_t n = 0;
    Mismatch bad;
    while (std::getline(f, line)) {
        const char kind = line[0];
        std::string rest = line.substr(2);
        if (kind == 'L' || kind == 'S') {
            const auto sp = rest.find(' ');
            const auto in = unhex(rest.substr(0, sp));
            const std::string want = rest.substr(sp + 1);
            std::string got;
            try {
                if (kind == 'L') {
                    const auto o = lha_expand(in.data(), in.size());
                    got = "ok " + lowhex(o.data(), o.size());
                } else {
                    got = "ok " + to_json(service_text(in.data(), in.size()));
                }
            } catch (const DataError& e) {
                got = std::string("err ") + e.what();
            }
            if (got != want) bad.add(line + "\n got: " + got);
        } else if (kind == 'N') {
            std::istringstream is(rest);
            int trim;
            std::string in, want;
            is >> trim >> in >> want;
            const auto b = unhex(in);
            const auto s = notice_text(b.data(), b.size(), trim != 0);
            if (lowhex(reinterpret_cast<const uint8_t*>(s.data()), s.size()) != want) bad.add(line + "\n got: " + s);
        }
        ++n;
    }
    std::printf("  text/LH5: %zu cases, mismatches %zu\n", n, bad.n);
    EXPECT_GT(n, 0u);
    EXPECT_EQ(bad.n, 0u);
}

TEST(JrTrainFuzz, DataMessageSequences) {
    REQUIRE_FUZZ();
    std::ifstream f(std::string(fuzz_dir()) + "/data.txt");
    std::string line;
    std::unique_ptr<DataDecoder> dec;
    std::vector<std::string> want, got;
    std::size_t seqs = 0, messages = 0;
    int frame = -1;
    Mismatch bad;
    while (std::getline(f, line)) {
        if (line.rfind("C ", 0) == 0) {
            dec = std::make_unique<DataDecoder>(line.substr(2) == "common" ? Format::Common : Format::Slotted);
            want.clear();
            got.clear();
            frame = -1;
        } else if (line.rfind("F ", 0) == 0) {
            std::istringstream is(line.substr(2));
            uint64_t off;
            std::string e;
            is >> off >> e;
            ++frame;
            for (const auto& m : dec->decode(env(e), off)) got.push_back(std::to_string(frame) + " " + to_json(m));
        } else if (line.rfind("M ", 0) == 0) {
            want.push_back(line.substr(2));
        } else if (line.rfind("E ", 0) == 0) {
            const std::string stats = std::to_string(dec->stats.valid_frames) + " " + std::to_string(dec->stats.corrected_frames) + " " +
                                      std::to_string(dec->stats.rejected_frames) + " " + std::to_string(dec->stats.rejected_messages);
            if (got != want || stats != line.substr(2)) {
                std::string detail = "sequence " + std::to_string(seqs) + " stats got " + stats + " want " + line.substr(2);
                for (std::size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
                    const std::string g = i < got.size() ? got[i] : "(none)", w = i < want.size() ? want[i] : "(none)";
                    if (g != w) {
                        detail += "\n got: " + g + "\nwant: " + w;
                        break;
                    }
                }
                bad.add(detail);
            }
            messages += want.size();
            ++seqs;
        }
    }
    std::printf("  data: %zu sequences, %zu messages, mismatches %zu\n", seqs, messages, bad.n);
    EXPECT_GT(seqs, 0u);
    EXPECT_EQ(bad.n, 0u);
}

uint64_t fnv(const uint8_t* b, std::size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

TEST(JrTrainFuzz, VoiceSynthesisStreams) {
    REQUIRE_FUZZ();
    const bool exact = test::same_platform(fuzz_dir());
    std::ifstream f(std::string(fuzz_dir()) + "/voice.txt");
    std::string line;
    std::unique_ptr<voice::Voice> v;
    std::vector<std::string> want, got;
    std::size_t streams = 0, outputs = 0;
    Mismatch bad;
    const auto record = [&] {
        while (auto o = v->read()) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "O %u %016llx", o->index,
                          static_cast<unsigned long long>(exact ? fnv(reinterpret_cast<const uint8_t*>(o->pcm.data()), 640) : 0));
            got.push_back(buf);
        }
    };
    const auto finish = [&] {
        if (!v) return;
        auto w = want;
        if (!exact)   // 別環境では PCM のハッシュを比べない(位置だけ)
            for (auto& l : w)
                if (l.rfind("O ", 0) == 0) l = l.substr(0, l.rfind(' ')) + " 0000000000000000";
        if (got != w) {
            std::string detail = "stream " + std::to_string(streams);
            for (std::size_t i = 0; i < std::max(got.size(), w.size()); ++i)
                if ((i < got.size() ? got[i] : "") != (i < w.size() ? w[i] : "")) {
                    detail += "\n got: " + (i < got.size() ? got[i] : "(none)") + "\nwant: " + (i < w.size() ? w[i] : "(none)");
                    break;
                }
            bad.add(detail);
        }
        ++streams;
    };
    while (std::getline(f, line)) {
        if (line == "B") {
            finish();
            v = std::make_unique<voice::Voice>();
            want.clear();
            got.clear();
        } else if (line.rfind("P ", 0) == 0) {
            std::istringstream is(line.substr(2));
            uint32_t index;
            std::string payload, result;
            is >> index >> payload >> result;
            std::optional<voice::Payload> p;
            if (payload != "-") {
                voice::Payload b{};
                const auto u = unhex(payload);
                std::memcpy(b.data(), u.data(), 20);
                p = b;
            }
            std::string r = "ok";
            try {
                v->push(index, p);
            } catch (const voice::VoiceError&) {
                r = "err";
            }
            got.push_back("P " + std::to_string(index) + " " + payload + " " + r);
            want.push_back(line);
            record();
        } else if (line.rfind("X ", 0) == 0) {
            std::string r = "ok";
            try {
                v->flush();
            } catch (const voice::VoiceError&) {
                r = "err";
            }
            got.push_back("X " + r);
            want.push_back(line);
            record();
        } else if (line.rfind("O ", 0) == 0) {
            want.push_back(line);
            ++outputs;
        }
    }
    finish();
    std::printf("  voice: %zu streams, %zu PCM frames, mismatches %zu%s\n", streams, outputs, bad.n, exact ? "" : " (positions only: other platform)");
    EXPECT_GT(streams, 0u);
    EXPECT_EQ(bad.n, 0u);
}

TEST(JrTrainFuzz, ExtremeIqThroughDsp) {
    REQUIRE_FUZZ();
    const bool exact = test::same_platform(fuzz_dir());
    std::ifstream f(std::string(fuzz_dir()) + "/dsp.txt");
    std::ifstream iqf(std::string(fuzz_dir()) + "/dsp_iq.cf32", std::ios::binary);
    std::string line;
    std::size_t cases = 0;
    Mismatch bad;
    while (std::getline(f, line)) {
        std::istringstream is(line.substr(2));
        std::size_t n, count;
        double offset;
        std::string dibits_hex, evm_s;
        is >> n >> offset >> count >> dibits_hex >> evm_s;
        std::vector<std::complex<float>> iq(n);
        iqf.read(reinterpret_cast<char*>(iq.data()), static_cast<std::streamsize>(n * 8));
        dsp::Demodulator d(240000.0, offset);
        std::vector<uint8_t> out;
        d.process(iq, out);   // 入力の区切り方に依らない(原本は乱数の長さで区切っている)
        std::vector<std::complex<float>> pts;
        const double evm = d.read_signal(pts, 256);
        const auto want = unhex(dibits_hex);
        std::size_t diff = out.size() == want.size() ? 0 : std::max(out.size(), want.size());
        if (out.size() == want.size())
            for (std::size_t i = 0; i < out.size(); ++i) diff += out[i] != want[i];
        const bool ok = exact ? (diff == 0 && std::abs(evm - std::stod(evm_s)) < 1e-9) : (out.size() == want.size() && diff <= want.size() / 50);
        if (!ok) bad.add("case " + std::to_string(cases) + " offset " + std::to_string(offset) + ": dibits differ " + std::to_string(diff) + " / " + std::to_string(want.size()) +
                         ", evm " + std::to_string(evm) + " want " + evm_s);
        ++cases;
    }
    std::printf("  dsp: %zu extreme IQ cases, mismatches %zu%s\n", cases, bad.n, exact ? "" : " (tolerant: other platform)");
    EXPECT_GT(cases, 0u);
    EXPECT_EQ(bad.n, 0u);
}

} // namespace
