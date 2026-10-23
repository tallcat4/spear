// spear-std-t98-decode — SigMF 録音を STD-T98 Receiver に流す headless 解析 (§10.4, §10.3 golden set)
//   spear-std-t98-decode <base> [--sync-ratio 0.2] [--sync-locked-ratio 0.6] [--sync-max-misses 3] [--freq-err 0] [--dump-ch N out.f32] [--pfb 64] [--wav base] [--hard] [--llr base] [--no-secret]
//   --freq-err: 入力を回して帯域中心を DC へ戻す量。App の録音(LO = 帯域中心 − 250 kHz)なら 250000(+ そのファイル固有の残留誤差)
// 出力: チャネルごとの電力/同期/フレーム統計、フレーム内容、推定周波数誤差。
// 秘話呼(SACCH call=1)は GUI と同じポリシー(5 バーストで最初の探索、5 バーストごとに再確認)で鍵を同期的に探索し、以後のフレームを
//   復号する(既定はソルバーのみ)。--no-secret で探索を止める。
//   AMBE の FEC は軟判定(App と同じ)。--hard で 9 byte の硬判定に戻す(比較用)。--llr base でチャネルごとの LLR 列(golden 用)を書く。
#include "ambe.hpp"
#include "receiver.hpp"
#include "secret/cracker.hpp"
#include "secret/tracker.hpp"
#include "spear/core/sigmf.hpp"
#include "spear/dsp/fir.hpp"

#include <array>
#include <chrono>
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
bool flag(int argc, char** argv, const char* key) {   // 値を取らないオプション(末尾でも効く)
    for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], key)) return true;
    return false;
}
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: spear-std-t98-decode <sigmf base> [--sync-ratio r] [--sync-locked-ratio r] [--sync-max-misses n] [--freq-err Hz] [--pfb N] [--dump-ch N --dump-file f [--dump-filtered]] [--log f] [--payloads f] [--raw2450 f] [--wav base] [--hard] [--llr base] [--no-secret] [--quiet]"); return 1; }
    const std::string base = argv[1];
    sigmf::Meta meta;
    std::string err;
    if (!sigmf::read(base, meta, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const double rate = meta.sample_rate;
    ReceiverConfig cfg;
    cfg.in_rate = rate;
    cfg.pfb_channels = std::stoi(arg(argc, argv, "--pfb", rate >= 4e6 ? "64" : "48"));
    cfg.sync_ratio = std::stod(arg(argc, argv, "--sync-ratio", "0.2"));
    cfg.freq_err_hz = std::stod(arg(argc, argv, "--freq-err", "0"));
    const int dump_ch = std::stoi(arg(argc, argv, "--dump-ch", "-1"));
    const bool quiet = flag(argc, argv, "--quiet");
    const bool hard = flag(argc, argv, "--hard");
    const char* llr_base = arg(argc, argv, "--llr", nullptr);
    std::map<int, std::FILE*> llr_files;
    cfg.sync_locked_ratio = std::stod(arg(argc, argv, "--sync-locked-ratio", "0.6"));
    cfg.sync_max_misses = std::stoi(arg(argc, argv, "--sync-max-misses", "3"));
    std::FILE* dump = dump_ch >= 0 ? std::fopen(arg(argc, argv, "--dump-file", "ch.f32"), "wb") : nullptr;
    // --log f: 入力ブロック(65536 sample = 16.4 ms)ごとに "idx in_dB ch01_dB sps01 act01 ch02_dB sps02 act02 ..." を 1 行(電力 / 同期の時系列解析用)
    std::FILE* logf = arg(argc, argv, "--log", nullptr) ? std::fopen(arg(argc, argv, "--log", ""), "w") : nullptr;
    // --wav f: 全チャネルの音声(AMBE → 8 kHz PCM)をチャネルごとに f を基にした f.chNN.wav へ書く(耳で確認する用)
    const char* wav_base = arg(argc, argv, "--wav", nullptr);
    // --payloads f: トラフィックフレームの TCH ペイロード(4 × 9 byte)を "ch sym hex" で 1 行ずつ(golden 生成用)
    std::FILE* payloads = arg(argc, argv, "--payloads", nullptr) ? std::fopen(arg(argc, argv, "--payloads", ""), "w") : nullptr;
    // --raw2450 f: ソルバー入力の FEC 後・秘話解除前 49 bit を保存する。
    std::ofstream raw2450(arg(argc, argv, "--raw2450", "/dev/null"));
    // チャネルの表示周波数: 録音中心 + freq_err(回転で DC に持ってきた周波数 = 帯域中心)+ チャネルオフセット
    const double band_center = (meta.captures.empty() ? 0.0 : meta.captures[0].frequency) + cfg.freq_err_hz;
    std::printf("rate=%.0f pfb=%d post1=%.0f center=%.6f MHz freq_err=%+.1f Hz (band center %.6f MHz)\n", rate, cfg.pfb_channels, cfg.pfb_channels * cfg.spacing_hz,
                meta.captures.empty() ? 0.0 : meta.captures[0].frequency / 1e6, cfg.freq_err_hz, band_center / 1e6);
    // 秘話の鍵探索(同期、既定はソルバーのみ)。--no-secret なら秘話呼はスクランブルのまま
    secret::Ffnn ffnn;
    secret::Hybrid hybrid;
    std::unique_ptr<secret::Cracker> cracker;
    if (!flag(argc, argv, "--no-secret")) {
        std::string e;
        if (secret::Cracker::Config().ai_fallback && (!ffnn.load_embedded(&e) || !hybrid.load_embedded(&e))) { std::fprintf(stderr, "secret: %s\n", e.c_str()); return 1; }
        cracker = std::make_unique<secret::Cracker>(ffnn, hybrid);
    }
    struct SecretCh { secret::Tracker tracker; bool call_secret = false; uint16_t pn_key = 0; secret::Pn196 pn{}; };
    std::vector<SecretCh> sec(static_cast<std::size_t>(cfg.num_channels));
    Receiver rx(cfg);

    // 周波数誤差推定: 活動中(同期中)のチャネルの discriminator 平均(Hz = 平均 × dev)
    std::vector<double> disc_sum(cfg.num_channels, 0), disc_n(cfg.num_channels, 0);
    std::vector<double> pk_pw(cfg.num_channels, -999);   // ピーク電力
    int frames = 0;
    Observer obs;
    // --dump-ch N [--dump-file f] [--dump-filtered]: そのチャネルの discriminator(既定)か RRC 後(×0.23)の 62.5 kHz 実数列を f32 で書く
    const bool dump_filtered = flag(argc, argv, "--dump-filtered");
    obs.discriminator = [&](int ch, std::span<const float> d) {
        if (!rx.metrics(ch).active) return;
        for (float v : d) { disc_sum[ch] += v; disc_n[ch] += 1; }
        if (dump && !dump_filtered && ch == dump_ch) std::fwrite(d.data(), sizeof(float), d.size(), dump);
    };
    obs.filtered = [&](int ch, std::span<const float> d) {
        if (dump && dump_filtered && ch == dump_ch) std::fwrite(d.data(), sizeof(float), d.size(), dump);
    };
    struct Voice { AmbeDecoder dec; std::vector<int16_t> pcm; uint64_t last_sym = 0; };
    std::map<int, Voice> voice;
    obs.frame = [&](const Frame& f) {
        ++frames;
        auto& sc = sec[static_cast<std::size_t>(f.channel)];
        if (f.rich.f == 0) { sc.call_secret = false; sc.tracker.on_clear(); }
        std::string secret_note;
        if (f.rich.f == 1 && f.tch_payload.size() == 36) {
            // 秘話判定(GUI と同じ: CRC OK の SACCH だけで更新、不良フレームは前の値を保つ)
            if (f.sacch.crc_ok) sc.call_secret = (f.sacch.call_stat == 1);
            if (!sc.call_secret) sc.tracker.on_clear();
            const bool keyed = sc.call_secret && sc.tracker.key() != 0;
            if (keyed && sc.pn_key != sc.tracker.key()) { sc.pn_key = sc.tracker.key(); sc.pn = secret::pn_sequence(sc.pn_key); }
            secret::Burst raw{};
            // AMBE の FEC は軟判定(App と同じ)。--hard なら 9 byte の硬判定で(比較用)
            auto block = [&](int k) { return std::span<const uint8_t, 9>(f.tch_payload.data() + k * 9, 9); };
            auto llr = [&](int k) { return std::span<const int8_t, 72>(f.tch_llr.data() + k * 72, 72); };
            for (int k = 0; k < 4; ++k) raw[static_cast<std::size_t>(k)] = secret::unpack_frame49(hard ? fec_demod_3600_to_2450(block(k)) : fec_soft_3600_to_2450(llr(k)));
            raw2450 << f.channel << ' ' << f.symbol_index;
            for (const auto& frame : raw) {
                const auto packed = secret::pack_frame49(frame);
                char hex[15]{};
                for (std::size_t i = 0; i < packed.size(); ++i) std::snprintf(hex + 2 * i, 3, "%02x", packed[i]);
                raw2450 << ' ' << hex;
            }
            raw2450 << '\n';
            if (wav_base) {
                auto& v = voice[f.channel];
                // 欠落フレーム(192 シンボル間隔でない)は無音で埋めて時間軸を保つ
                if (v.last_sym && f.symbol_index > v.last_sym + 192) v.pcm.resize(v.pcm.size() + 640 * ((f.symbol_index - v.last_sym) / 192 - 1), 0);
                v.last_sym = f.symbol_index;
                for (int k = 0; k < 4; ++k) {
                    std::array<int16_t, 160> pcm{};
                    const uint8_t* ks = keyed ? secret::keystream_d(sc.pn, k) : nullptr;
                    if (hard) v.dec.decode_3600(block(k), pcm, ks); else v.dec.decode_3600_soft(llr(k), pcm, ks);
                    v.pcm.insert(v.pcm.end(), pcm.begin(), pcm.end());
                }
            }
            if (sc.call_secret && cracker) {
                secret::Tracker::Request rq;
                if (sc.tracker.on_secret_burst(raw, &rq)) {
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto r = cracker->resolve(rq.current_key, rq.bursts);
                    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    sc.tracker.on_result(rq.session, r.key);
                    secret_note = "  SECRET key=" + std::to_string(r.key) + " (" + secret::to_string(r.source) + ", " + std::to_string(dt).substr(0, 4) + " s)";
                }
            }
        }
        if (llr_base && f.rich.f == 1 && f.sacch.crc_ok) {   // チャネルごとの LLR 列(72 × int8 / ブロック)。golden 用
            auto& fp = llr_files[f.channel];
            if (!fp) fp = std::fopen((std::string(llr_base) + ".ch" + (f.channel + 1 < 10 ? "0" : "") + std::to_string(f.channel + 1) + ".llr").c_str(), "wb");
            if (fp) std::fwrite(f.tch_llr.data(), 1, f.tch_llr.size(), fp);
        }
        if (payloads && f.rich.f == 1 && f.sacch.crc_ok) {
            std::fprintf(payloads, "%d %llu ", f.channel, (unsigned long long)f.symbol_index);
            for (uint8_t b : f.tch_payload) std::fprintf(payloads, "%02x", b);
            std::fprintf(payloads, "\n");
        }
        if (quiet) return;
        std::printf("FRAME ch%02d (%.5f MHz) sym=%llu in=%llu sse=%.2f%s RICH F=%d M=%d parity=%s", f.channel + 1,
                    (band_center + rx.channel_offset_hz(f.channel)) / 1e6,
                    (unsigned long long)f.symbol_index, (unsigned long long)f.input_sample_index, f.sync_sse, f.sync_ok ? "" : " FLY", f.rich.f, f.rich.m, f.rich.parity_ok ? "ok" : "BAD");
        if (f.rich.f == 0) std::printf("  PICH csm=%s crc=%s err=%d", f.pich.csm.c_str(), f.pich.crc_ok ? "ok" : "BAD", f.pich.bit_errors);
        else if (f.rich.f == 1) std::printf("  SACCH msg=%d call=%d user=%d maker=%d crc=%s err=%d%s%s", f.sacch.msg_type, f.sacch.call_stat, f.sacch.user_code, f.sacch.maker_code, f.sacch.crc_ok ? "ok" : "BAD", f.sacch.bit_errors,
                                            sc.call_secret ? (sc.tracker.key() ? "  [secret K " : "  [secret") : "", sc.call_secret ? (sc.tracker.key() ? (std::to_string(sc.tracker.key()) + "]").c_str() : "]") : "");
        std::printf("%s\n", secret_note.c_str());
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
        for (int c = 0; c < cfg.num_channels; ++c) {
            pk_pw[c] = std::max(pk_pw[c], rx.metrics(c).power_db);
        }
        if (logf) {
            double pw = 0;
            for (std::size_t i = 0; i < n; ++i) pw += std::norm(iq[i]);
            std::fprintf(logf, "%llu %.2f", (unsigned long long)idx, 10.0 * std::log10(pw / static_cast<double>(n) + 1e-20));
            for (int c = 0; c < cfg.num_channels; ++c) std::fprintf(logf, " %.2f %.4f %d", rx.metrics(c).power_db, rx.metrics(c).sps, rx.metrics(c).active ? 1 : 0);
            std::fprintf(logf, "\n");
        }
        idx += n;
        if (f.eof()) break;
    }
    if (dump) std::fclose(dump);
    if (logf) std::fclose(logf);
    if (payloads) std::fclose(payloads);
    for (auto& [ch, fp] : llr_files) if (fp) std::fclose(fp);
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
    std::printf("\n%-4s %-12s %8s %6s %8s %7s %7s %8s %6s %6s %6s %s\n", "ch", "freq[MHz]", "peak dB", "sync", "best_sse", "frames", "sacch", "pich_ok", "locks", "fly", "drop", "est freq err / csm");
    for (int c = 0; c < cfg.num_channels; ++c) {
        const auto& m = rx.metrics(c);
        if (m.sync_detections == 0 && m.frames == 0) continue;   // 一度も同期語を見なかったチャネルは省く
        const double fe = disc_n[c] > 0 ? disc_sum[c] / disc_n[c] * cfg.fsk_dev_hz : 0;
        std::printf("%-4d %-12.5f %8.1f %6llu %8.2f %7llu %7llu %8llu %6llu %6llu %6llu %+.0f Hz %s\n", c + 1,
                    (band_center + rx.channel_offset_hz(c)) / 1e6, pk_pw[c],
                    (unsigned long long)m.sync_detections, m.best_sse, (unsigned long long)m.frames, (unsigned long long)m.sacch_ok,
                    (unsigned long long)m.pich_ok, (unsigned long long)m.locks, (unsigned long long)m.flywheel_frames, (unsigned long long)m.dropped_frames, fe, m.csm.c_str());
    }
    std::printf("total frames %d, samples %llu (%.1f s)\n", frames, (unsigned long long)idx, static_cast<double>(idx) / rate);
    return 0;
}
