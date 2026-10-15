// spear-jr-train-decode — 録音を JR TRAIN RADIO の Receiver に流す headless 解析(§10.4)。docs/apps/jr_train.md
//   spear-jr-train-decode <sigmf base> [--preset east|central|west] [--center Hz] [--threads N]
//                         [--frames-dir DIR] [--export-wav out.wav] [--quiet]
//   spear-jr-train-decode --raw-sc16 FILE --rate Hz --capture-center Hz [同上]
// 出力: チャネルごとの搬送波ロック・周波数誤差・EVM・フレーム同期の統計。--quiet でなければ同期したフレームを 1 行ずつ。
//   --frames-dir: チャネル・連続区間ごとに frames.bin(48 byte/フレーム)+ indices.txt を書く。原本の
//                 `jr-receiver decode-frames DIR OUT slotted|common` でそのまま音声・データに復号できる(format.txt に種別)
//   --export-wav: 回転・間引き後の帯域 IQ(work_rate、ステレオ float32、左 I・右 Q)。原本の
//                 `jr-receiver decode-wave out.wav <帯域中心 Hz> OUT <preset>` に渡せる(C++ と Rust の突き合わせ用)
#include "presets.hpp"
#include "receiver.hpp"
#include "spear/core/sigmf.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace spear;
using namespace spear::jr_train;

namespace {
const char* arg(int argc, char** argv, const char* key, const char* def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return def;
}
bool flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], key)) return true;
    return false;
}

// RIFF/WAVE ステレオ float32(WAVE_FORMAT_IEEE_FLOAT)。サイズは閉じるときに書く
class WavWriter {
public:
    WavWriter(const std::string& path, uint32_t rate) : f_(path, std::ios::binary), rate_(rate) {
        if (f_) header(0);
    }
    bool ok() const { return static_cast<bool>(f_); }
    void write(std::span<const cf32> iq) {
        f_.write(reinterpret_cast<const char*>(iq.data()), static_cast<std::streamsize>(iq.size() * sizeof(cf32)));
        bytes_ += iq.size() * sizeof(cf32);
    }
    uint64_t bytes() const { return bytes_; }
    void close() {
        if (!f_) return;
        f_.seekp(0);
        header(bytes_);
        f_.close();
    }

private:
    void put32(uint32_t v) { f_.write(reinterpret_cast<const char*>(&v), 4); }
    void put16(uint16_t v) { f_.write(reinterpret_cast<const char*>(&v), 2); }
    void header(uint64_t data) {
        const auto d = static_cast<uint32_t>(std::min<uint64_t>(data, 0xffffffffu - 36));
        f_.write("RIFF", 4);
        put32(36 + d);
        f_.write("WAVEfmt ", 8);
        put32(16);
        put16(3);   // IEEE float
        put16(2);
        put32(rate_);
        put32(rate_ * 8);
        put16(8);
        put16(32);
        f_.write("data", 4);
        put32(d);
    }
    std::ofstream f_;
    uint32_t rate_;
    uint64_t bytes_ = 0;
};

// 原本 decode-frames の入力(1 チャネル・1 連続区間)。bit_offset が 384 の倍数で進む間は同じ区間
struct FrameStream {
    std::ofstream bin, idx;
    uint64_t epoch = 0, origin = 0, last = 0;
    int segment = -1;
    bool open = false;
};
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: spear-jr-train-decode <sigmf base> [--preset east|central|west] [--center Hz] [--threads N] [--frames-dir DIR] [--export-wav out.wav] [--quiet]\n"
                  "       spear-jr-train-decode --raw-sc16 FILE --rate Hz --capture-center Hz [...]");
        return 1;
    }
    const Preset* preset = find_preset(arg(argc, argv, "--preset", "east"));
    if (!preset) {
        std::fprintf(stderr, "unknown preset\n");
        return 1;
    }
    const double band_center = std::stod(arg(argc, argv, "--center", std::to_string(preset->center_hz).c_str()));
    const bool quiet = flag(argc, argv, "--quiet");

    // 入力: SigMF(spear の録音)または生の sc16
    std::string data_path;
    double rate = 0, capture_center = 0;
    DataType dtype = DataType::ComplexInt16;
    std::vector<uint64_t> breaks;   // この sample の直前で連続性が切れている(sidecar の discontinuities)
    if (const char* raw = arg(argc, argv, "--raw-sc16", nullptr)) {
        data_path = raw;
        rate = std::stod(arg(argc, argv, "--rate", "0"));
        capture_center = std::stod(arg(argc, argv, "--capture-center", "0"));
    } else {
        sigmf::Meta meta;
        std::string err;
        const std::string base = argv[1];
        if (!sigmf::read(base, meta, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        data_path = sigmf::data_path(base);
        rate = meta.sample_rate;
        dtype = meta.dtype;
        capture_center = meta.captures.empty() ? 0.0 : meta.captures[0].frequency;
        if (meta.captures.size() > 1) std::fprintf(stderr, "warning: %zu captures (retune); using the first frequency\n", meta.captures.size());
        for (const auto& d : meta.discontinuities) breaks.push_back(d.file_sample);
    }
    if (rate <= 0 || capture_center <= 0) {
        std::fprintf(stderr, "missing sample rate or capture center\n");
        return 1;
    }
    if (dtype != DataType::ComplexInt16 && dtype != DataType::ComplexFloat32) {
        std::fprintf(stderr, "unsupported datatype (ci16_le / cf32_le only)\n");
        return 1;
    }

    ReceiverConfig cfg;
    cfg.in_rate = rate;
    cfg.rotate_hz = band_center - capture_center;
    cfg.threads = std::stoi(arg(argc, argv, "--threads", "0"));
    for (const auto& c : preset->channels) cfg.channel_offsets_hz.push_back(c.frequency_hz - band_center);
    Receiver rx(cfg);
    std::printf("preset %s  band center %.6f MHz  capture %.6f MHz (rotate %+.1f Hz)  rate %.0f -> work %.0f (/%zu)  threads %d\n", preset->id.c_str(), band_center / 1e6,
                capture_center / 1e6, cfg.rotate_hz, rate, rx.work_rate(), rx.decim(), rx.threads());

    std::unique_ptr<WavWriter> wav;
    if (const char* w = arg(argc, argv, "--export-wav", nullptr)) {
        wav = std::make_unique<WavWriter>(w, static_cast<uint32_t>(std::lround(rx.work_rate())));
        if (!wav->ok()) {
            std::fprintf(stderr, "cannot write %s\n", w);
            return 1;
        }
    }
    const char* frames_dir = arg(argc, argv, "--frames-dir", nullptr);
    std::vector<FrameStream> streams(preset->channels.size());
    if (frames_dir) {
        std::error_code ec;
        std::filesystem::create_directories(frames_dir, ec);
    }
    auto stream_dir = [&](std::size_t c, int seg) {
        const auto& ch = preset->channels[c];
        char name[64];
        std::snprintf(name, sizeof name, "%.0fHz-%02d", ch.frequency_hz, seg);
        return std::filesystem::path(frames_dir) / name;
    };

    Observer o;
    o.band = [&](std::span<const cf32> iq, uint64_t) {
        if (wav) wav->write(iq);
    };
    o.frame = [&](const Frame& f) {
        const auto c = static_cast<std::size_t>(f.channel);
        if (frames_dir) {
            auto& s = streams[c];
            const bool cont = s.open && f.epoch == s.epoch && f.bit_offset > s.last && (f.bit_offset - s.origin) % protocol::kFrameBits == 0;
            if (!cont) {
                s.bin.close();
                s.idx.close();
                ++s.segment;
                const auto dir = stream_dir(c, s.segment);
                std::filesystem::create_directories(dir);
                s.bin.open(dir / "frames.bin", std::ios::binary);
                s.idx.open(dir / "indices.txt");
                std::ofstream(dir / "format.txt") << (preset->channels[c].service == Service::Common ? "common" : "slotted") << "\n";
                s.epoch = f.epoch;
                s.origin = f.bit_offset;
                s.open = true;
            }
            s.last = f.bit_offset;
            s.bin.write(reinterpret_cast<const char*>(f.received.data()), static_cast<std::streamsize>(f.received.size()));
            s.idx << (f.bit_offset - s.origin) / protocol::kFrameBits << "\n";
        }
        if (quiet || !f.sync_matched) return;
        std::printf("FRAME %-8s %.5f MHz epoch %llu bit %llu in %llu markers-err %u  ", preset->channels[c].short_label.c_str(), preset->channels[c].frequency_hz / 1e6,
                    static_cast<unsigned long long>(f.epoch), static_cast<unsigned long long>(f.bit_offset), static_cast<unsigned long long>(f.input_sample_index), f.marker_errors);
        for (uint8_t b : f.received) std::printf("%02x", b);
        std::printf("\n");
    };
    rx.set_observer(o);

    std::ifstream in(data_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", data_path.c_str());
        return 1;
    }
    const std::size_t block = 65536;
    std::vector<sc16> raw16(block);
    std::vector<cf32> iq(block);
    uint64_t idx = 0;
    std::size_t next_break = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::size_t want = block;
        while (next_break < breaks.size() && breaks[next_break] <= idx) {
            if (breaks[next_break] > 0) rx.discontinuity();
            ++next_break;
        }
        if (next_break < breaks.size()) want = std::min<std::size_t>(want, breaks[next_break] - idx);
        std::size_t n = 0;
        if (dtype == DataType::ComplexInt16) {
            in.read(reinterpret_cast<char*>(raw16.data()), static_cast<std::streamsize>(want * sizeof(sc16)));
            n = static_cast<std::size_t>(in.gcount()) / sizeof(sc16);
            for (std::size_t i = 0; i < n; ++i) iq[i] = cf32(raw16[i].real() / 32768.f, raw16[i].imag() / 32768.f);
        } else {
            in.read(reinterpret_cast<char*>(iq.data()), static_cast<std::streamsize>(want * sizeof(cf32)));
            n = static_cast<std::size_t>(in.gcount()) / sizeof(cf32);
        }
        if (n == 0) break;
        rx.process(std::span<const cf32>(iq.data(), n), idx);
        idx += n;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (wav) {
        wav->close();
        if (wav->bytes() > 0xffffffffull - 36) std::fprintf(stderr, "warning: WAV exceeds 4 GiB; the header is clamped (split the recording)\n");
    }
    rx.update_signal();

    std::printf("\n%-8s %-11s %-6s %6s %9s %7s %7s %7s %6s %s\n", "ch", "freq[MHz]", "type", "carrier", "offset", "EVM%", "frames", "synced", "epochs", "lock");
    for (int c = 0; c < rx.num_channels(); ++c) {
        const auto& ch = preset->channels[static_cast<std::size_t>(c)];
        const auto& s = rx.status(c);
        std::printf("%-8s %-11.5f %-6s %6s %+8.1f %7.2f %7llu %7llu %6llu %s\n", ch.short_label.c_str(), ch.frequency_hz / 1e6, to_string(ch.service),
                    s.snap.carrier_locked ? "LOCK" : "-", s.snap.frequency_offset_hz, s.evm_percent, static_cast<unsigned long long>(s.frames),
                    static_cast<unsigned long long>(s.synchronized_frames), static_cast<unsigned long long>(s.epoch + 1), s.frame_locked ? "FRAME" : "");
    }
    const double seconds = static_cast<double>(idx) / rate;
    std::printf("samples %llu (%.2f s), processed in %.2f s (%.0f%% of real time)\n", static_cast<unsigned long long>(idx), seconds, elapsed, seconds > 0 ? elapsed / seconds * 100 : 0);
    if (wav) std::printf("wrote %s (%.0f S/s stereo float32, center %.0f Hz)\n", arg(argc, argv, "--export-wav", ""), rx.work_rate(), band_center);
    return 0;
}
