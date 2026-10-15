// フレーム復号・データ電文・パイプラインのテスト。参照ベクトル(tests/vectors/protocol)は tests/refgen(bin protocol)が
// 原本の Rust 実装に、符号化した有効なフレームとビット誤りを入れたものを通して書き出したもの。
#include "pipeline.hpp"
#include "protocol/coding.hpp"
#include "protocol/data.hpp"
#include "protocol/voice_frame.hpp"
#include "platform.hpp"
#include "receiver.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace spear::jr_train;
using namespace spear::jr_train::protocol;

std::string path(const std::string& n) { return std::string(JR_TRAIN_VECTORS_DIR) + "/protocol/" + n; }

std::vector<std::string> lines(const std::string& name) {
    std::ifstream f(path(name));
    std::vector<std::string> out;
    std::string l;
    while (std::getline(f, l)) out.push_back(l);
    return out;
}

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

TEST(JrTrainProtocol, VoiceFrameMatchesRust) {
    const auto ls = lines("voice_frames.txt");
    ASSERT_GT(ls.size(), 400u);
    std::size_t accepted = 0;
    for (const auto& l : ls) {
        std::istringstream is(l);
        std::string e, payload, corrected;
        is >> e >> payload >> corrected;
        const auto s = decode_voice(env(e));
        if (payload == "-") {
            EXPECT_FALSE(s) << l;
        } else {
            ASSERT_TRUE(s) << l;
            EXPECT_EQ(lowhex(s->payload.data(), 20), payload);
            EXPECT_EQ(std::to_string(s->corrected_bits), corrected);
            ++accepted;
        }
    }
    EXPECT_GT(accepted, 250u);
}

TEST(JrTrainProtocol, DataFrameMatchesRust) {
    for (const auto& l : lines("data_frames.txt")) {
        std::istringstream is(l);
        std::string e, info, corrected;
        is >> e >> info >> corrected;
        const auto r = decode_data_frame(env(e));
        if (info == "-") {
            EXPECT_FALSE(r) << l;
        } else {
            ASSERT_TRUE(r) << l;
            EXPECT_EQ(lowhex(r->first.data(), 28), info);
            EXPECT_EQ(std::to_string(r->second), corrected);
        }
    }
}

TEST(JrTrainProtocol, DataMessagesMatchRust) {
    const auto ls = lines("data_cases.txt");
    ASSERT_FALSE(ls.empty());
    std::unique_ptr<DataDecoder> dec;
    std::vector<std::string> expected, got;
    std::string name;
    int frame = -1;
    for (const auto& l : ls) {
        if (l.rfind("case ", 0) == 0) {
            std::istringstream is(l.substr(5));
            std::string fmt;
            is >> name >> fmt;
            dec = std::make_unique<DataDecoder>(fmt == "common" ? Format::Common : Format::Slotted);
            expected.clear();
            got.clear();
            frame = -1;
        } else if (l.rfind("f ", 0) == 0) {
            std::istringstream is(l.substr(2));
            uint64_t off;
            std::string e;
            is >> off >> e;
            ++frame;
            for (const auto& m : dec->decode(env(e), off)) got.push_back(std::to_string(frame) + " " + to_json(m));
        } else if (l.rfind("m ", 0) == 0) {
            expected.push_back(l.substr(2));
        } else if (l.rfind("s ", 0) == 0) {
            SCOPED_TRACE(name);
            ASSERT_EQ(got.size(), expected.size());
            for (std::size_t i = 0; i < got.size(); ++i) EXPECT_EQ(got[i], expected[i]);
            std::istringstream is(l.substr(2));
            uint64_t v, c, r, rm;
            is >> v >> c >> r >> rm;
            EXPECT_EQ(dec->stats.valid_frames, v);
            EXPECT_EQ(dec->stats.corrected_frames, c);
            EXPECT_EQ(dec->stats.rejected_frames, r);
            EXPECT_EQ(dec->stats.rejected_messages, rm);
        }
    }
}

TEST(JrTrainProtocol, TextAndLh5MatchRust) {
    std::size_t n = 0;
    for (const auto& l : lines("text_cases.txt")) {
        std::istringstream is(l);
        std::string kind;
        is >> kind;
        std::string rest;
        std::getline(is, rest);
        rest.erase(0, 1);
        SCOPED_TRACE(l.substr(0, 120));
        if (kind == "lha") {
            const auto sp = rest.find(' ');
            const auto in = unhex(rest.substr(0, sp));
            const std::string outcome = rest.substr(sp + 1);
            try {
                const auto out = lha_expand(in.data(), in.size());
                EXPECT_EQ("ok " + lowhex(out.data(), out.size()), outcome);
            } catch (const DataError& e) {
                EXPECT_EQ(std::string("err ") + e.what(), outcome);
            }
        } else if (kind == "notice") {
            std::istringstream r(rest);
            int trim;
            std::string in, out;
            r >> trim >> in >> out;
            const auto b = unhex(in);
            const auto s = notice_text(b.data(), b.size(), trim != 0);
            EXPECT_EQ(lowhex(reinterpret_cast<const uint8_t*>(s.data()), s.size()), out);
        } else if (kind == "jis") {
            std::istringstream r(rest);
            std::string rc, status;
            r >> rc >> status;
            std::string tail;
            std::getline(r, tail);
            if (!tail.empty()) tail.erase(0, 1);
            const auto b = unhex(rc);
            try {
                const auto s = jis(b[0], b[1]);
                EXPECT_EQ(status, "ok");
                EXPECT_EQ(lowhex(reinterpret_cast<const uint8_t*>(s.data()), s.size()), tail);
            } catch (const DataError& e) {
                EXPECT_EQ(status, "err");
                EXPECT_EQ(tail, e.what());
            }
        } else if (kind == "service") {
            const auto sp = rest.find(' ');
            const auto in = unhex(rest.substr(0, sp));
            const std::string outcome = rest.substr(sp + 1);
            try {
                EXPECT_EQ("ok " + to_json(service_text(in.data(), in.size())), outcome);
            } catch (const DataError& e) {
                EXPECT_EQ(std::string("err ") + e.what(), outcome);
            }
        }
        ++n;
    }
    EXPECT_GT(n, 100u);
}

// 原本 coding.rs / data/mod.rs のテストの一部
TEST(JrTrainProtocol, CrcAndGaloisBasics) {
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(crc16(s), 0x29b1);   // CRC-16/CCITT-FALSE の検査値
    const auto& gf = Galois::get();
    for (int a = 1; a < 256; ++a) EXPECT_EQ(gf.div(gf.mul(static_cast<uint8_t>(a), 0x53), 0x53), a);
}

// ---- パイプライン(音声合成を含む)----

struct PipeCase {
    std::string name;
    Format format;
    std::vector<std::string> expected, got;
};

TEST(JrTrainPipeline, FramesVoiceAndDataMatchRust) {
    const auto ls = lines("pipeline.txt");
    ASSERT_FALSE(ls.empty());
    std::unique_ptr<Pipeline> pipe;
    std::vector<std::string> expected, got;
    std::string name;
    auto record = [&](const std::vector<Audio>& audio) {
        for (const auto& a : audio)
            got.push_back("a " + std::to_string(a.epoch) + " " + std::to_string(a.index) + " " + lowhex(reinterpret_cast<const uint8_t*>(a.pcm.data()), 640));
    };
    auto finish = [&]() {
        if (!pipe) return;
        record(pipe->flush());
        const auto& st = pipe->snapshot();
        got.push_back("status " + std::to_string(st.frames) + " " + std::to_string(st.synchronized_frames) + " " + std::to_string(st.voice_frames) + " " +
                      std::to_string(st.rejected_voice) + " " + std::to_string(st.corrected_bits) + " " + std::to_string(st.audio_frames) + " " +
                      std::to_string(st.epoch) + " " + std::to_string(st.data.valid_frames));
        for (const auto& r : st.messages) got.push_back("h " + std::to_string(r.receptions) + " " + to_json(r.message));
        if (st.operation_monitor) got.push_back("op " + std::to_string(st.operation_monitor->receptions) + " " + to_json(st.operation_monitor->summary));
        SCOPED_TRACE(name);
        ASSERT_EQ(got.size(), expected.size());
        // フレームからの復号は整数だけなので常に一致する。音声の PCM は libm に依存するので、別の環境では位置(epoch / index)だけ比べる
        const bool exact = test::same_platform(std::string(JR_TRAIN_VECTORS_DIR) + "/protocol");
        const auto key = [&](const std::string& l) { return (!exact && l.rfind("a ", 0) == 0) ? l.substr(0, l.rfind(' ')) : l; };
        std::size_t mismatched = 0;
        for (std::size_t i = 0; i < got.size(); ++i)
            if (key(got[i]) != key(expected[i]) && ++mismatched <= 5) ADD_FAILURE() << "line " << i << "\n got: " << got[i].substr(0, 200) << "\nwant: " << expected[i].substr(0, 200);
        EXPECT_EQ(mismatched, 0u);
    };
    for (const auto& l : ls) {
        if (l.rfind("case ", 0) == 0) {
            finish();
            std::istringstream is(l.substr(5));
            std::string fmt;
            is >> name >> fmt;
            const Format format = fmt == "common" ? Format::Common : Format::Slotted;
            const double freq = format == Format::Common ? 352'625'000.0 : 352'600'000.0;
            pipe = std::make_unique<Pipeline>(ChannelConfig{std::to_string(static_cast<uint64_t>(freq)), name, freq, format}, 240000.0, 352603125.0);
            expected.clear();
            got.clear();
        } else if (l.rfind("f ", 0) == 0) {
            std::istringstream is(l.substr(2));
            uint64_t off;
            std::string e;
            is >> off >> e;
            protocol::Frame f;
            f.bit_offset = off;
            f.sync_matched = true;
            f.received = env(e);
            record(pipe->frame(f, [&](const FrameEvent& ev) {
                got.push_back("e " + std::to_string(ev.epoch) + " " + std::to_string(ev.index) + " " + (ev.corrected_bits ? std::to_string(*ev.corrected_bits) : "-") +
                              " " + std::to_string(ev.messages.size()));
                for (const auto& m : ev.messages) got.push_back("m " + to_json(m));
            }));
        } else {
            expected.push_back(l);
        }
    }
    finish();
}

// ---- 通し: 帯域 IQ → Receiver(DSP・同期・音声・データ)----

TEST(JrTrainPipeline, EndToEndIqMatchesRust) {
    const auto ls = lines("e2e.txt");
    ASSERT_FALSE(ls.empty());
    double rate = 0, center = 0;
    std::size_t samples = 0;
    std::vector<std::pair<uint64_t, Format>> chans;
    std::vector<std::vector<std::string>> expected;
    for (const auto& l : ls) {
        std::istringstream is(l);
        std::string k;
        is >> k;
        if (k == "rate") is >> rate;
        else if (k == "center") is >> center;
        else if (k == "samples") is >> samples;
        else if (k == "case") {
            uint64_t f;
            std::string fmt;
            is >> f >> fmt;
            chans.emplace_back(f, fmt == "common" ? Format::Common : Format::Slotted);
            expected.emplace_back();
        } else if (!expected.empty()) {
            expected.back().push_back(l);
        }
    }
    std::ifstream f(path("e2e_240k.sc16"), std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    ASSERT_EQ(raw.size(), samples * 4);
    std::vector<std::complex<float>> iq(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        int16_t re, im;
        std::memcpy(&re, &raw[4 * i], 2);
        std::memcpy(&im, &raw[4 * i + 2], 2);
        iq[i] = {static_cast<float>(re) / 32768.f, static_cast<float>(im) / 32768.f};
    }
    // Receiver(全チャネル並列、ブロックは原本と同じ 4096)の出力を原本の行形式にする
    ReceiverConfig cfg;
    cfg.in_rate = rate;
    std::vector<ChannelConfig> configs;
    for (const auto& [freq, fmt] : chans) configs.push_back({std::to_string(freq), "e2e", static_cast<double>(freq), fmt});
    cfg.channels = configs;
    cfg.band_center_hz = center;
    cfg.threads = 3;
    Receiver rx(cfg);
    std::vector<std::vector<std::string>> got(chans.size());
    Observer o;
    o.frame = [&](int ch, const FrameEvent& ev, const FrameTiming&) {
        got[static_cast<std::size_t>(ch)].push_back("e " + std::to_string(ev.epoch) + " " + std::to_string(ev.index) + " " + std::to_string(ev.frame.bit_offset) + " " +
                                                    (ev.corrected_bits ? std::to_string(*ev.corrected_bits) : "-") + " " + std::to_string(ev.messages.size()));
        for (const auto& m : ev.messages) got[static_cast<std::size_t>(ch)].push_back("m " + to_json(m));
    };
    o.audio = [&](int ch, const Audio& a) {
        got[static_cast<std::size_t>(ch)].push_back("a " + std::to_string(a.epoch) + " " + std::to_string(a.index) + " " + lowhex(reinterpret_cast<const uint8_t*>(a.pcm.data()), 640));
    };
    rx.set_observer(o);
    for (std::size_t i = 0; i < samples; i += 4096) rx.process(std::span<const std::complex<float>>(iq).subspan(i, std::min<std::size_t>(4096, samples - i)), i);
    rx.flush();
    for (std::size_t c = 0; c < chans.size(); ++c) {
        const auto& st = rx.snapshot(static_cast<int>(c));   // 原本と同じく snapshot で電文の統計を反映する
        got[c].push_back("status " + std::to_string(st.frames) + " " + std::to_string(st.synchronized_frames) + " " + std::to_string(st.voice_frames) + " " +
                         std::to_string(st.rejected_voice) + " " + std::to_string(st.corrected_bits) + " " + std::to_string(st.audio_frames) + " " +
                         std::to_string(st.epoch) + " " + std::to_string(st.data.valid_frames));
        SCOPED_TRACE("channel " + std::to_string(chans[c].first));
        if (test::same_platform(std::string(JR_TRAIN_VECTORS_DIR) + "/protocol")) {
            ASSERT_EQ(got[c].size(), expected[c].size());
            std::size_t mismatched = 0;
            for (std::size_t i = 0; i < got[c].size(); ++i)
                if (got[c][i] != expected[c][i] && ++mismatched <= 5) ADD_FAILURE() << "line " << i << "\n got: " << got[c][i].substr(0, 160) << "\nwant: " << expected[c][i].substr(0, 160);
            EXPECT_EQ(mismatched, 0u);
        } else {
            // 別の環境: 電文は同じ(誤り訂正で吸収される)、音声フレームの数はほぼ同じ
            const auto pick = [](const std::vector<std::string>& v, const char* prefix) {
                std::vector<std::string> out;
                for (const auto& l : v)
                    if (l.rfind(prefix, 0) == 0) out.push_back(l);
                return out;
            };
            EXPECT_EQ(pick(got[c], "m "), pick(expected[c], "m "));
            EXPECT_NEAR(static_cast<double>(pick(got[c], "a ").size()), static_cast<double>(pick(expected[c], "a ").size()), 3.0);
        }
    }
}

} // namespace

// ---- 原本 data/mod.rs と data/operation.rs の単体テストの移植 ----
namespace {
using namespace spear::jr_train;
using namespace spear::jr_train::protocol;

uint16_t sum16(const std::vector<uint8_t>& b, std::size_t from, std::size_t to) {
    uint16_t s = 0;
    for (std::size_t i = from; i < to; ++i) s = static_cast<uint16_t>(s + b[i]);
    return s;
}
void put_be(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xff));
}

// 共用波のパケット → データフレーム(原本 operation.rs のテストの frames と同じ符号化)
std::vector<std::pair<uint64_t, std::array<uint8_t, 48>>> encode_common(const std::vector<Packet>& packets) {
    const auto& gf = Galois::get();
    std::vector<uint8_t> gen{1};
    for (int root = 0; root < 12; ++root) {
        std::vector<uint8_t> next(gen.size() + 1, 0);
        for (std::size_t i = 0; i < gen.size(); ++i) {
            next[i] ^= gen[i];
            next[i + 1] ^= gf.mul(gen[i], gf.powers[static_cast<std::size_t>(root)]);
        }
        gen = next;
    }
    std::vector<std::pair<uint64_t, std::array<uint8_t, 48>>> out;
    for (const auto& p : packets) {
        const std::size_t count = (p.bytes.size() + 26) / 27;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t len = std::min<std::size_t>(27, p.bytes.size() - 27 * i);
            std::array<uint8_t, 42> word{};
            word[0] = count == 1 ? static_cast<uint8_t>(0xc0 | len)
                      : i == 0   ? static_cast<uint8_t>(0x80 | (count - 1))
                      : i + 1 == count ? static_cast<uint8_t>(0x40 | len)
                                       : static_cast<uint8_t>(count - i - 1);
            std::copy_n(p.bytes.begin() + static_cast<std::ptrdiff_t>(27 * i), len, word.begin() + 1);
            const uint16_t crc = crc16(std::span<const uint8_t>(word.data(), 28));
            word[28] = static_cast<uint8_t>(crc >> 8);
            word[29] = static_cast<uint8_t>(crc & 0xff);
            const auto info = word;
            for (std::size_t j = 0; j < 30; ++j) {
                const uint8_t c = word[j];
                for (std::size_t k = 0; k < 13; ++k) word[j + k] ^= gf.mul(c, gen[k]);
            }
            std::copy_n(info.begin(), 30, word.begin());
            std::array<uint8_t, 48> e{};
            for (std::size_t m = 0; m < 3; ++m) {
                e[16 * m] = static_cast<uint8_t>(kMarkers[m] >> 8);
                e[16 * m + 1] = static_cast<uint8_t>(kMarkers[m] & 0xff);
            }
            std::array<uint8_t, 336> pn;
            pn.fill(1);
            for (std::size_t b = 0; b < 336; ++b) {
                if (b >= 9) pn[b] = pn[b - 9] ^ pn[b - 5];
                const std::size_t pos = 128 * (b / 112) + 16 + b % 112;
                e[pos / 8] |= static_cast<uint8_t>((((word[b / 8] >> (b % 8)) & 1) ^ pn[b]) << (7 - pos % 8));
            }
            out.emplace_back(p.first + 384 * i, e);
        }
    }
    return out;
}

std::vector<uint8_t> table(uint8_t delayed_line0 = 0x83) {
    std::vector<uint8_t> b{0x87};
    for (uint8_t i = 0; i < 7; ++i) {
        b.insert(b.end(), {7, 0, 0x3b, 0x6e, 0x38, 0x33, 0x23, static_cast<uint8_t>(0x41 + i), 0x21, 0x21});   // 試験Ａ…Ｇ
        b.insert(b.end(), {0x80, static_cast<uint8_t>(0x80 | (7 + i)), 0, 0xf0, 0x12, 3, i == 0 ? delayed_line0 : uint8_t{0x83}, 0xa8, 0x82, 0x81, 0x80});
    }
    return b;
}
std::vector<uint8_t> archive(const std::vector<uint8_t>& data) {   // 定数リテラルのブロックを 1 byte ずつ
    std::vector<uint8_t> bits;
    for (uint8_t byte : data)
        for (auto [v, n] : std::initializer_list<std::pair<int, int>>{{1, 16}, {0, 5}, {0, 5}, {0, 9}, {byte, 9}, {0, 4}, {0, 4}})
            for (int k = n - 1; k >= 0; --k) bits.push_back(static_cast<uint8_t>((v >> k) & 1));
    std::vector<uint8_t> packed((bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i) packed[i / 8] |= static_cast<uint8_t>(bits[i] << (7 - i % 8));
    std::vector<uint8_t> a(28, 0);
    a[0] = 26;
    std::memcpy(&a[2], "-lh5-", 5);
    for (int k = 0; k < 4; ++k) {
        a[7 + static_cast<std::size_t>(k)] = static_cast<uint8_t>(packed.size() >> (8 * k));
        a[11 + static_cast<std::size_t>(k)] = static_cast<uint8_t>(data.size() >> (8 * k));
    }
    a[20] = 1;
    a[21] = 1;
    a[22] = 't';
    uint16_t crc = 0;
    for (uint8_t v : data) {
        crc ^= v;
        for (int k = 0; k < 8; ++k) crc = static_cast<uint16_t>((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
    }
    a[23] = static_cast<uint8_t>(crc & 0xff);
    a[24] = static_cast<uint8_t>(crc >> 8);
    uint8_t s = 0;
    for (std::size_t i = 2; i < a.size(); ++i) s = static_cast<uint8_t>(s + a[i]);
    a[1] = s;
    a.insert(a.end(), packed.begin(), packed.end());
    return a;
}
std::vector<uint8_t> body(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> b{1, 0, 1, 0x19, 0x02, 0xaa, 0x7f, 0, 0xff};
    put_be(b, sum16(data, 0, data.size()));
    const auto a = archive(data);
    b.insert(b.end(), a.begin(), a.end());
    return b;
}
std::vector<Packet> packets(const std::vector<uint8_t>& b, uint16_t serial) {
    const std::size_t total = (b.size() + 239) / 240;
    std::vector<Packet> out;
    uint64_t offset = 0;
    for (std::size_t i = 0; i < total; ++i) {
        const std::size_t len = std::min<std::size_t>(240, b.size() - 240 * i);
        std::vector<uint8_t> bytes{0x22, static_cast<uint8_t>(14 + len), 5, 0, 0, 0, 0x10, 2};
        put_be(bytes, serial);
        bytes.insert(bytes.end(), {1, 1, static_cast<uint8_t>(((i + 1) << 4) | total), 0xc9, 1, 0x21});
        bytes.insert(bytes.end(), b.begin() + static_cast<std::ptrdiff_t>(240 * i), b.begin() + static_cast<std::ptrdiff_t>(240 * i + len));
        const uint64_t first = offset;
        offset += (bytes.size() + 26) / 27 * 384;
        out.push_back({bytes, first, offset - 384});
    }
    return out;
}
std::vector<Message> run(DataDecoder& d, const std::vector<Packet>& ps) {
    std::vector<Message> out;
    for (const auto& [off, e] : encode_common(ps))
        for (auto& m : d.decode(e, off)) out.push_back(std::move(m));
    return out;
}

TEST(JrTrainDataOriginal, DirectoryCountIsNotASecondsComponent) {
    DataDecoder d(Format::Common);
    for (int count : {0, 1, 9, 13, 15, 16, 118}) {
        std::vector<uint8_t> b(13, 0);
        b[0] = 0x21;
        b[2] = 4;
        b.insert(b.end(), {0x25, 0x03, 0x14, 0x09, 0x26, static_cast<uint8_t>(count)});
        for (int id = 0; id < count; ++id) put_be(b, static_cast<uint16_t>(0xa000 + id));
        put_be(b, sum16(b, 13, b.size()));
        b[1] = static_cast<uint8_t>(b.size() - 2);
        if (b.size() > 257) continue;   // 1 パケット(257 byte)を超える数はフレームにできない(原本のテストは common() を直接呼ぶ)
        const auto ms = run(d, {{b, 0, 0}});
        ASSERT_EQ(ms.size(), 1u) << count;
        EXPECT_EQ(ms[0].kind, "MessageDirectory");
        EXPECT_EQ(ms[0].timestamp.value_or(""), "2025-03-14T09:26:00");
        EXPECT_EQ(ms[0].raw_hex, hex_upper(b.data(), b.size()));
        const auto& ids = ms[0].fields[0].value;
        EXPECT_EQ(count == 0 ? 0 : static_cast<int>(std::count(ids.begin(), ids.end(), ' ')) + 1, count);
        if (count) EXPECT_EQ(ids.substr(0, 4), "A000");
    }
}

TEST(JrTrainDataOriginal, OperationTableReachesStructuredJson) {
    DataDecoder d(Format::Common);
    const auto ms = run(d, packets(body(table()), 0x1234));
    ASSERT_EQ(ms.size(), 1u);
    EXPECT_EQ(ms[0].kind, "OperationSummary");
    EXPECT_FALSE(ms[0].timestamp);
    ASSERT_TRUE(ms[0].operation_summary);
    const auto& s = *ms[0].operation_summary;
    ASSERT_EQ(s.rows.size(), 7u);
    EXPECT_EQ(s.rows[0].line, "試験Ａ");
    EXPECT_EQ(s.rows[0].on_line.value_or(0), 40);
    EXPECT_EQ(s.rows[0].train_no.value_or(""), "12C");
    EXPECT_TRUE(s.prefix_sum_matches);
    EXPECT_NE(to_json(ms[0]).find("\"delay_classes\":[2,1,0]"), std::string::npos);
}

TEST(JrTrainDataOriginal, MissingReorderedMixedOrCorruptedPartsCannotPublish) {
    const auto p = packets(body(table()), 0x1234);
    ASSERT_GE(p.size(), 5u);
    for (const auto& order : std::vector<std::vector<int>>{{1, 2, 3, 4}, {0, 2, 3, 4}, {0, 2, 1, 3, 4}}) {
        OperationAssembler a;
        for (int i : order) EXPECT_FALSE(a.accept(p[static_cast<std::size_t>(i)]));
    }
    auto mixed = p;
    mixed[1].bytes[9] ^= 1;
    OperationAssembler a;
    for (const auto& part : mixed) EXPECT_FALSE(a.accept(part));
    auto frames = encode_common(p);
    frames[2].second.fill(0);
    DataDecoder d(Format::Common);
    std::size_t n = 0;
    for (const auto& [off, e] : frames) n += d.decode(e, off).size();
    EXPECT_EQ(n, 0u);
    EXPECT_GT(d.stats.rejected_frames, 0u);
    EXPECT_EQ(run(d, p).size(), 1u);   // 正しい先頭からやり直せば復帰する
}

TEST(JrTrainDataOriginal, InvalidArchivesAndLayoutsAreRejectedAndUnknownIsNotZero) {
    const auto original = body(table());
    for (std::size_t n = 0; n < original.size(); ++n) EXPECT_THROW(decode_operation(original.data(), n), DataError) << n;
    auto damaged = original;
    damaged[12] ^= 1;
    EXPECT_THROW(decode_operation(damaged.data(), damaged.size()), DataError);   // 書庫ヘッダの検査和
    damaged = original;
    damaged[34] ^= 1;
    uint8_t s = 0;
    for (std::size_t i = 13; i < 39; ++i) s = static_cast<uint8_t>(s + damaged[i]);
    damaged[12] = s;   // ヘッダは直して、展開後の CRC で落ちること
    EXPECT_THROW(decode_operation(damaged.data(), damaged.size()), DataError);
    auto rows = table();
    rows[0] = 0x88;
    EXPECT_THROW(decode_operation(body(rows).data(), body(rows).size()), DataError);
    rows = table();
    rows[1] = 8;
    EXPECT_THROW(decode_operation(body(rows).data(), body(rows).size()), DataError);
    rows = table();
    rows[17] = 0;
    rows[21] = 0x80;
    rows[14] = 0xff;
    rows[15] = 0xff;
    const auto b = body(rows);
    const auto decoded = decode_operation(b.data(), b.size());
    EXPECT_FALSE(decoded.rows[0].delayed);
    EXPECT_EQ(decoded.rows[0].delay_classes[2].value_or(99), 0);
    EXPECT_FALSE(decoded.rows[0].train_no);
    auto prefix = original;
    prefix[9] ^= 1;
    EXPECT_FALSE(decode_operation(prefix.data(), prefix.size()).prefix_sum_matches);
}

TEST(JrTrainDataOriginal, RetransmissionsCountIdenticalContentAndReplaceChangedTables) {
    DataDecoder d(Format::Common);
    auto message = run(d, packets(body(table()), 0x1234)).at(0);
    std::optional<OperationReception> state;
    OperationReception::update(state, message, "first");
    message.identity = "4567";
    OperationReception::update(state, message, "second");
    EXPECT_EQ(state->first_received_at, "first");
    EXPECT_EQ(state->last_received_at, "second");
    EXPECT_EQ(state->receptions, 2u);
    message.timestamp = "2024-12-31T23:59:59";
    OperationReception::update(state, message, "third");
    EXPECT_EQ(state->receptions, 3u);
    const auto changed = body(table(0x80));
    message.operation_summary = decode_operation(changed.data(), changed.size());
    OperationReception::update(state, message, "fifth");
    EXPECT_EQ(state->receptions, 1u);
    EXPECT_EQ(state->summary.rows[0].delayed.value_or(99), 0);
    EXPECT_EQ(state->first_received_at, "fifth");
}

TEST(JrTrainDataOriginal, OpaquePrefixIsPreservedWithoutTimestamp) {
    const auto original = body(table());
    for (const auto& prefix : std::vector<std::array<uint8_t, 6>>{{0, 0, 0, 0, 0, 0}, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, {153, 131, 142, 137, 154, 181}}) {
        auto input = original;
        std::copy(prefix.begin(), prefix.end(), input.begin() + 3);
        DataDecoder d(Format::Common);
        const auto ms = run(d, packets(input, 0x1234));
        ASSERT_EQ(ms.size(), 1u);
        EXPECT_FALSE(ms[0].timestamp);
        EXPECT_EQ(ms[0].raw_hex, hex_upper(input.data(), input.size()));
        EXPECT_EQ(ms[0].operation_summary->prefix_hex, hex_upper(input.data(), 11));
        EXPECT_EQ(d.stats.rejected_messages, 0u);
    }
}

TEST(JrTrainDataOriginal, PlainServiceInformationDecodes) {
    std::vector<uint8_t> content{0x25, 0x03, 0x14, 0x09, 0x26, 0, 0, 0, 0, 0, 0, 0};
    std::vector<uint8_t> b{1, 0, 1, 0, 1, 1, 0x99, 0x99};
    put_be(b, static_cast<uint16_t>(sum16(b, 0, b.size()) + sum16(content, 0, content.size())));
    b.insert(b.end(), content.begin(), content.end());
    auto p = packets(b, 1);
    p[0].bytes[0] = 0x21;
    p[0].bytes[13] = 0x46;
    DataDecoder d(Format::Common);
    const auto ms = run(d, p);
    ASSERT_EQ(ms.size(), 1u);
    EXPECT_EQ(ms[0].kind, "ServiceInformation");
    EXPECT_EQ(ms[0].timestamp.value_or(""), "2025-03-14T09:26:00");
    EXPECT_EQ(to_json(ms[0]).find("operation_summary"), std::string::npos);
}
} // namespace
