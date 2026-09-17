// spear-std-t98-decode — SigMF 録音を STD-T98 Receiver に流す headless 解析 (§11, §12.3 golden set)
//   spear-std-t98-decode <base> [--squelch -40] [--sync-ratio 0.2] [--freq-err 0] [--dump-ch N out.f32] [--pfb 64] [--wav base]
// 出力: チャネルごとの電力/同期/フレーム統計、フレーム内容、推定周波数誤差。
#include "ambe.hpp"
#include "receiver.hpp"
#include "spear/core/sigmf.hpp"
#include "spear/dsp/fir.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <numbers>
#include <vector>

using namespace spear;
using namespace spear::std_t98;

namespace {
const char* arg(int argc, char** argv, const char* key, const char* def) {
    for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return def;
}
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: spear-std-t98-decode <sigmf base> [--squelch dB] [--sync-ratio r] [--freq-err Hz] [--pfb N] [--dump-ch N out.f32] [--payloads f] [--wav base] [--quiet]"); return 1; }
    const std::string base = argv[1];
    sigmf::Meta meta;
    std::string err;
    if (!sigmf::read(base, meta, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const double rate = meta.sample_rate;
    ReceiverConfig cfg;
    cfg.in_rate = rate;
    cfg.pfb_channels = std::stoi(arg(argc, argv, "--pfb", rate >= 4e6 ? "64" : "48"));
    cfg.squelch_db = std::stod(arg(argc, argv, "--squelch", "-40"));
    cfg.sync_ratio = std::stod(arg(argc, argv, "--sync-ratio", "0.2"));
    cfg.freq_err_hz = std::stod(arg(argc, argv, "--freq-err", "0"));
    const int dump_ch = std::stoi(arg(argc, argv, "--dump-ch", "-1"));
    const bool quiet = arg(argc, argv, "--quiet", nullptr) != nullptr;
    std::FILE* dump = dump_ch >= 0 ? std::fopen(arg(argc, argv, "--dump-file", "ch.f32"), "wb") : nullptr;
    // --wav f: 全チャネルの音声(AMBE → 8 kHz PCM)をチャネルごとに f を基にした f.chNN.wav へ書く(耳で確認する用)
    const char* wav_base = arg(argc, argv, "--wav", nullptr);
    // --payloads f: トラフィックフレームの TCH ペイロード(4 × 9 byte)を "ch sym hex" で 1 行ずつ(golden 生成用)
    std::FILE* payloads = arg(argc, argv, "--payloads", nullptr) ? std::fopen(arg(argc, argv, "--payloads", ""), "w") : nullptr;
    std::printf("rate=%.0f pfb=%d post1=%.0f center=%.6f MHz freq_err=%+.1f Hz\n", rate, cfg.pfb_channels, cfg.pfb_channels * cfg.spacing_hz,
                meta.captures.empty() ? 0.0 : meta.captures[0].frequency / 1e6, cfg.freq_err_hz);
    Receiver rx(cfg);

    // 周波数誤差推定: 開いているチャネルの discriminator 平均(Hz = 平均 × dev)
    std::vector<double> disc_sum(cfg.num_channels, 0), disc_n(cfg.num_channels, 0);
    std::vector<double> pk_pw(cfg.num_channels, -999);
    int frames = 0;
    Observer obs;
    obs.discriminator = [&](int ch, std::span<const float> d) {
        if (!rx.metrics(ch).open) return;
        for (float v : d) { disc_sum[ch] += v; disc_n[ch] += 1; }
        if (dump && ch == dump_ch) std::fwrite(d.data(), sizeof(float), d.size(), dump);
    };
    struct Voice { AmbeDecoder dec; std::vector<int16_t> pcm; uint64_t last_sym = 0; };
    std::map<int, Voice> voice;
    obs.frame = [&](const Frame& f) {
        ++frames;
        if (wav_base && f.rich.f == 1 && f.tch_payload.size() == 36) {
            auto& v = voice[f.channel];
            // 欠落フレーム(192 シンボル間隔でない)は無音で埋めて時間軸を保つ
            if (v.last_sym && f.symbol_index > v.last_sym + 192) v.pcm.resize(v.pcm.size() + 640 * ((f.symbol_index - v.last_sym) / 192 - 1), 0);
            v.last_sym = f.symbol_index;
            for (int k = 0; k < 4; ++k) {
                std::array<int16_t, 160> pcm{};
                v.dec.decode_3600(std::span<const uint8_t, 9>(f.tch_payload.data() + k * 9, 9), pcm);
                v.pcm.insert(v.pcm.end(), pcm.begin(), pcm.end());
            }
        }
        if (payloads && f.rich.f == 1 && f.sacch.crc_ok) {
            std::fprintf(payloads, "%d %llu ", f.channel, (unsigned long long)f.symbol_index);
            for (uint8_t b : f.tch_payload) std::fprintf(payloads, "%02x", b);
            std::fprintf(payloads, "\n");
        }
        if (quiet) return;
        std::printf("FRAME ch%02d (%.5f MHz) sym=%llu in=%llu sse=%.2f RICH F=%d M=%d parity=%s", f.channel + 1,
                    (meta.captures.empty() ? 0.0 : meta.captures[0].frequency + rx.channel_offset_hz(f.channel)) / 1e6,
                    (unsigned long long)f.symbol_index, (unsigned long long)f.input_sample_index, f.sync_sse, f.rich.f, f.rich.m, f.rich.parity_ok ? "ok" : "BAD");
        if (f.rich.f == 0) std::printf("  PICH csm=%s crc=%s err=%d", f.pich.csm.c_str(), f.pich.crc_ok ? "ok" : "BAD", f.pich.bit_errors);
        else if (f.rich.f == 1) std::printf("  SACCH msg=%d call=%d user=%d maker=%d crc=%s err=%d", f.sacch.msg_type, f.sacch.call_stat, f.sacch.user_code, f.sacch.maker_code, f.sacch.crc_ok ? "ok" : "BAD", f.sacch.bit_errors);
        std::printf("\n");
    };
    rx.set_observer(obs);

    std::ifstream f(sigmf::data_path(base), std::ios::binary);
    const std::size_t block = 65536;
    std::vector<sc16> raw(block);
    std::vector<cf32> iq(block);
    uint64_t idx = 0;
    while (f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(block * sizeof(sc16))) || f.gcount() > 0) {
        const std::size_t n = static_cast<std::size_t>(f.gcount()) / sizeof(sc16);
        for (std::size_t i = 0; i < n; ++i) {
            iq[i] = cf32(raw[i].real() / 32768.f, raw[i].imag() / 32768.f);
        }
        rx.process(std::span<const cf32>(iq.data(), n), idx);
        for (int c = 0; c < cfg.num_channels; ++c) pk_pw[c] = std::max(pk_pw[c], rx.metrics(c).power_db);
        idx += n;
        if (f.eof()) break;
    }
    if (dump) std::fclose(dump);
    if (payloads) std::fclose(payloads);
    for (auto& [ch, v] : voice) {
        const std::string name = std::string(wav_base) + ".ch" + (ch + 1 < 10 ? "0" : "") + std::to_string(ch + 1) + ".wav";
        std::FILE* w = std::fopen(name.c_str(), "wb");
        if (!w) continue;
        const uint32_t data_bytes = static_cast<uint32_t>(v.pcm.size() * 2), rate8 = 8000, byte_rate = 16000, riff = 36 + data_bytes;
        const uint16_t fmt_pcm = 1, ch1 = 1, align = 2, bits = 16; const uint32_t fmt_len = 16;
        std::fwrite("RIFF", 1, 4, w); std::fwrite(&riff, 4, 1, w); std::fwrite("WAVEfmt ", 1, 8, w); std::fwrite(&fmt_len, 4, 1, w);
        std::fwrite(&fmt_pcm, 2, 1, w); std::fwrite(&ch1, 2, 1, w); std::fwrite(&rate8, 4, 1, w); std::fwrite(&byte_rate, 4, 1, w);
        std::fwrite(&align, 2, 1, w); std::fwrite(&bits, 2, 1, w); std::fwrite("data", 1, 4, w); std::fwrite(&data_bytes, 4, 1, w);
        std::fwrite(v.pcm.data(), 2, v.pcm.size(), w); std::fclose(w);
        std::printf("wrote %s (%.2f s)\n", name.c_str(), v.pcm.size() / 8000.0);
    }
    std::printf("\n%-4s %-12s %8s %6s %8s %7s %7s %8s %s\n", "ch", "freq[MHz]", "peak dB", "sync", "best_sse", "frames", "sacch", "pich_ok", "est freq err / csm");
    for (int c = 0; c < cfg.num_channels; ++c) {
        const auto& m = rx.metrics(c);
        if (pk_pw[c] < cfg.squelch_db && m.sync_detections == 0) continue;
        const double fe = disc_n[c] > 0 ? disc_sum[c] / disc_n[c] * cfg.fsk_dev_hz : 0;
        std::printf("%-4d %-12.5f %8.1f %6llu %8.2f %7llu %7llu %8llu %+.0f Hz %s\n", c + 1,
                    (meta.captures.empty() ? 0.0 : meta.captures[0].frequency + rx.channel_offset_hz(c)) / 1e6, pk_pw[c],
                    (unsigned long long)m.sync_detections, m.best_sse, (unsigned long long)m.frames, (unsigned long long)m.sacch_ok,
                    (unsigned long long)m.pich_ok, fe, m.csm.c_str());
    }
    std::printf("total frames %d, samples %llu (%.1f s)\n", frames, (unsigned long long)idx, static_cast<double>(idx) / rate);
    return 0;
}
