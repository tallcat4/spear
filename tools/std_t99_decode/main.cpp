// spear-std-t99-decode — SigMF 録音(radio.rx)から STD-T99 Annex 2 の 1 チャネルを復号する(App と同じ Monitor)。
//   spear-std-t99-decode <sigmf base> [--ch N | --freq Hz] [--save dir] [--wav file] [--quiet]
// チャネルを省くと録音の中心 + 250 kHz(App の LO オフセット)に最も近いチャネル。標準出力: フレームごとの JSON 行(Session::json)、
// 最後に送信ごとの要約。--save は App の SAVE と同じ 4 ファイル(.burst / .llr / .jsonl / .wav)、--wav は復号音声だけ。
#include "channels.hpp"
#include "monitor.hpp"
#include "storage.hpp"
#include "voice/tokuder.hpp"

#include "spear/core/sigmf.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace spear;
using namespace spear::std_t99;

namespace {
const char* arg(int argc, char** argv, const char* key, const char* def) {
    for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return def;
}
bool flag(int argc, char** argv, const char* key) {
    for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], key)) return true;
    return false;
}
void write_wav(const std::string& path, const std::vector<float>& pcm) {
    std::ofstream o(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    o.write("RIFF", 4); u32(static_cast<uint32_t>(36 + pcm.size() * 2)); o.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16); o.write("data", 4); u32(static_cast<uint32_t>(pcm.size() * 2));
    for (float v : pcm) u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f))));
}
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: spear-std-t99-decode <sigmf base> [--ch N | --freq Hz] [--save dir] [--wav file] [--quiet]"); return 1; }
    const std::string base = argv[1];
    sigmf::Meta meta;
    std::string err;
    if (!sigmf::read(base, meta, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const double rate = meta.sample_rate;
    const double center = meta.captures.empty() ? 0 : meta.captures.front().frequency;
    double freq = 0;
    if (const char* f = arg(argc, argv, "--freq", nullptr)) freq = std::stod(f);
    else if (const char* c = arg(argc, argv, "--ch", nullptr)) freq = channel_freq_hz(std::stoi(c));
    else freq = channel_freq_hz(find_channel(center + 250e3).value_or(kCallingChannel));
    const int ch = find_channel(freq).value_or(0);
    const bool quiet = flag(argc, argv, "--quiet");
    std::fprintf(stderr, "%s: %.0f sps, center %.6f MHz, CH %d %.6f MHz (shift %+.0f Hz)\n", base.c_str(), rate, center / 1e6, ch, freq / 1e6, freq - center);

    Recorder saver;
    if (const char* dir = arg(argc, argv, "--save", nullptr)) std::fprintf(stderr, "saving %s\n", saver.start(dir, ch).c_str());
    std::vector<float> audio, pcm;
    tokuder::Decoder dec(tokuder::model());
    uint64_t frames = 0, crc = 0, voice = 0;
    MonitorObserver o;
    o.frame = [&](const FrameEvent& e) {
        ++frames; crc += e.frame.crc_ok(); voice += e.frame.voice();
        if (!quiet) std::printf("%s\n", e.json.c_str());
        saver.frame(e.frame, e.json, "");
    };
    o.voice = [&](const VoiceCommand& v) {
        pcm.clear();
        if (v.kind == VoiceCommand::Kind::Flush) dec.flush(pcm); else dec.push(v.info.data(), pcm);
        audio.insert(audio.end(), pcm.begin(), pcm.end());
        saver.audio(pcm);
    };
    Monitor mon(rate, freq - center, o);

    std::ifstream f(sigmf::data_path(base), std::ios::binary);
    const std::size_t block = 65536;
    std::vector<sc16> raw(block);
    std::vector<cf32> iq(block);
    uint64_t index = 0;
    while (f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(block * sizeof(sc16))) || f.gcount() > 0) {
        const std::size_t n = static_cast<std::size_t>(f.gcount()) / sizeof(sc16);
        for (std::size_t i = 0; i < n; ++i) iq[i] = cf32(raw[i].real() / 32768.f, raw[i].imag() / 32768.f);
        mon.process(std::span<const cf32>(iq.data(), n), index);
        index += n;
    }
    mon.flush_voice();
    saver.stop();
    if (const char* w = arg(argc, argv, "--wav", nullptr)) write_wav(w, audio);

    const auto st = mon.status();
    std::fprintf(stderr, "%.1f s: %llu frames, %llu with CRC, %llu voice, %.2f s audio, offset %+.0f Hz\n", mon.seconds(),
                 static_cast<unsigned long long>(frames), static_cast<unsigned long long>(crc), static_cast<unsigned long long>(voice),
                 static_cast<double>(audio.size()) / 8000, st.freq_offset_hz);
    const auto& h = mon.calls().history();
    for (auto it = h.rbegin(); it != h.rend(); ++it) {
        const auto& c = *it;
        std::fprintf(stderr, "#%llu  %s -> %s  %s  %s  name \"%s\"%s  control %d  frames %llu%s%s\n", static_cast<unsigned long long>(c.number),
                     c.caller.value_or("?").c_str(), c.destination().c_str(), c.call_type().c_str(), c.kind >= 0 ? message_name(c.kind) : "",
                     c.name.c_str(), c.name_failed ? " (check failed)" : "", c.call_control, static_cast<unsigned long long>(c.frames),
                     c.position ? ("  " + c.position->str()).c_str() : "", c.gps_time ? ("  " + c.gps_time->str()).c_str() : "");
    }
    return 0;
}
