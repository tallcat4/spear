// spear-jr-train-decode — 録音を JR TRAIN RADIO の Receiver に流す headless 解析(§10.4)。docs/apps/jr_train.md
//   spear-jr-train-decode <sigmf base> [--preset east|central|west] [--center Hz] [--threads N] [--quiet]
//                         [--decode-out DIR] [--save DIR] [--frames-dir DIR] [--export-wav out.wav] [--seconds S]
//   spear-jr-train-decode --raw-sc16 FILE --rate Hz --capture-center Hz [同上]
//   spear-jr-train-decode --wav FILE --capture-center Hz [同上]     原本と同じ IQ WAV(ステレオ float32 / PCM16、左 I・右 Q)
// 受信するチャネル: --presets-file JSON(原本と同じ形式)、--routes id,id(路線から選ぶ。共用波も受信、--no-common で外す)
// 出力: チャネルごとの搬送波・EVM・フレーム同期・音声・データの統計。--quiet でなければ電文を 1 行ずつ。
//   --decode-out: 原本の `jr-receiver decode-wave` と同じ形式(チャネル ID ごとに audio.wav / data.jsonl / messages.json、
//                 全体の report.json)。原本の出力と diff で比べられる
//   --save:       GUI の保存と同じ形式(recording-…/ に WAV・raw-frames.jsonl・data-messages.jsonl)
//   --frames-dir: チャネル・連続区間ごとに frames.bin(48 byte/フレーム)+ indices.txt(原本の `decode-frames` の入力)+ samples.txt(各フレームの radio.rx sample index(概算)と音声の訂正ビット数、不合格は -1)
//   --export-wav: 回転・間引き後の帯域 IQ(work_rate、ステレオ float32、左 I・右 Q)。原本の `decode-wave` に渡せる
#include "presets.hpp"
#include "receiver.hpp"
#include "spear/core/sigmf.hpp"
#include "storage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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
class IqWavWriter {
public:
    IqWavWriter(const std::string& path, uint32_t rate) : f_(path, std::ios::binary), rate_(rate) {
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
    std::ofstream bin, idx, smp;   // smp: フレームの radio.rx sample index(FrameTiming。概算 ±数シンボル)
    uint64_t epoch = 0, origin = 0, last = 0;
    int segment = -1;
    bool open = false;
};

// 原本 decode-wave の Output(チャネルごと: audio.wav / data.jsonl)
struct DecodeOut {
    std::unique_ptr<PcmWave> wave;
    std::ofstream data;
    std::optional<std::pair<uint64_t, uint32_t>> last;
    void audio(const Audio& a) {
        if (last && last->first == a.epoch) {
            if (a.index <= last->second) throw std::runtime_error("Non-monotonic PCM index");
            wave->silence(static_cast<uint64_t>(a.index - last->second - 1) * 320);
        }
        wave->write(a.pcm.data(), a.pcm.size());
        last = std::make_pair(a.epoch, a.index);
    }
};

std::string status_json(const PipelineStatus& st) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"iq_samples\":%llu,\"frames\":%llu,\"synchronized_frames\":%llu,\"voice_frames\":%llu,\"rejected_voice\":%llu,\"corrected_bits\":%llu,"
                  "\"audio_frames\":%llu,\"discontinuities\":%llu,\"frame_locked\":%s,\"carrier_locked\":%s,\"frequency_error_hz\":%.3f,\"evm_percent\":%.3f,"
                  "\"input_dbfs\":%.2f,\"data\":{\"valid_frames\":%llu,\"corrected_frames\":%llu,\"rejected_frames\":%llu,\"rejected_messages\":%llu},\"epoch\":%llu}",
                  static_cast<unsigned long long>(st.iq_samples), static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.synchronized_frames),
                  static_cast<unsigned long long>(st.voice_frames), static_cast<unsigned long long>(st.rejected_voice), static_cast<unsigned long long>(st.corrected_bits),
                  static_cast<unsigned long long>(st.audio_frames), static_cast<unsigned long long>(st.discontinuities), st.frame_locked ? "true" : "false",
                  st.carrier_locked ? "true" : "false", st.frequency_error_hz, st.evm_percent, st.input_dbfs, static_cast<unsigned long long>(st.data.valid_frames),
                  static_cast<unsigned long long>(st.data.corrected_frames), static_cast<unsigned long long>(st.data.rejected_frames),
                  static_cast<unsigned long long>(st.data.rejected_messages), static_cast<unsigned long long>(st.epoch));
    return buf;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: spear-jr-train-decode <sigmf base> [--preset east|central|west] [--center Hz] [--threads N] [--seconds S] [--quiet]\n"
                  "                             [--decode-out DIR] [--save DIR] [--frames-dir DIR] [--export-wav out.wav]\n"
                  "       spear-jr-train-decode --raw-sc16 FILE --rate Hz --capture-center Hz [...]");
        return 1;
    }
    std::vector<Preset> catalog = presets();
    if (const char* file = arg(argc, argv, "--presets-file", nullptr)) {
        try {
            catalog = load_presets_file(file);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
    const Preset* preset = nullptr;
    const std::string preset_id = arg(argc, argv, "--preset", catalog.front().id.c_str());
    for (const auto& p : catalog)
        if (p.id == preset_id) preset = &p;
    if (!preset) {
        std::fprintf(stderr, "unknown preset %s\n", preset_id.c_str());
        return 1;
    }
    std::vector<ChannelDef> channels = preset->channels;
    if (const char* routes = arg(argc, argv, "--routes", nullptr)) {
        std::vector<std::string> ids;
        std::string cur;
        for (const char* c = routes;; ++c) {
            if (*c == ',' || *c == 0) {
                if (!cur.empty()) ids.push_back(cur);
                cur.clear();
                if (*c == 0) break;
            } else {
                cur += *c;
            }
        }
        channels = channels_for_routes(*preset, ids, !flag(argc, argv, "--no-common"));
        if (channels.empty()) {
            std::fprintf(stderr, "no channels for routes %s (see the preset's routes)\n", routes);
            return 1;
        }
    }
    const double band_center = std::stod(arg(argc, argv, "--center", std::to_string(preset->center_hz).c_str()));
    const bool quiet = flag(argc, argv, "--quiet");

    // 入力: SigMF(spear の録音)または生の sc16
    std::string data_path;
    double rate = 0, capture_center = 0;
    DataType dtype = DataType::ComplexInt16;
    std::vector<uint64_t> breaks;   // この sample の直前で連続性が切れている(sidecar の discontinuities)
    uint64_t data_offset = 0, total_samples = UINT64_MAX;
    if (const char* wav_in = arg(argc, argv, "--wav", nullptr)) {
        // 原本 wave.rs IqWave と同じ条件: RIFF 長 = ファイル長、fmt と data が 1 つずつ、ステレオ float32 または PCM16、9.6 k〜20 MS/s
        std::ifstream w(wav_in, std::ios::binary);
        std::vector<uint8_t> h(12);
        const auto le16 = [](const uint8_t* b) { return static_cast<uint32_t>(b[0] | (b[1] << 8)); };
        const auto le32 = [](const uint8_t* b) { return static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16)) | (static_cast<uint32_t>(b[3]) << 24); };
        const auto size = std::filesystem::exists(wav_in) ? std::filesystem::file_size(wav_in) : 0;
        if (!w.read(reinterpret_cast<char*>(h.data()), 12) || std::memcmp(h.data(), "RIFF", 4) || std::memcmp(h.data() + 8, "WAVE", 4) || 8ull + le32(h.data() + 4) != size) {
            std::fprintf(stderr, "expected a RIFF WAVE IQ file whose RIFF length equals the file length\n");
            return 1;
        }
        std::vector<uint8_t> fmt;
        uint64_t pos = 12, data_size = 0;
        bool have_data = false;
        while (pos + 8 <= size) {
            uint8_t ch[8];
            w.seekg(static_cast<std::streamoff>(pos));
            w.read(reinterpret_cast<char*>(ch), 8);
            const uint64_t n = le32(ch + 4);
            if (pos + 8 + n > size) {
                std::fprintf(stderr, "truncated WAV chunk\n");
                return 1;
            }
            if (!std::memcmp(ch, "fmt ", 4) && n >= 16 && fmt.empty()) {
                fmt.resize(16);
                w.read(reinterpret_cast<char*>(fmt.data()), 16);
            } else if (!std::memcmp(ch, "data", 4) && !have_data) {
                data_offset = pos + 8;
                data_size = n;
                have_data = true;
            }
            pos += 8 + n + (n & 1);
        }
        if (fmt.empty() || !have_data) {
            std::fprintf(stderr, "missing WAV format or IQ data\n");
            return 1;
        }
        const uint32_t format = le16(fmt.data()), channels_n = le16(fmt.data() + 2), bits = le16(fmt.data() + 14);
        const uint32_t bytes = bits / 8;
        if (channels_n != 2 || !((format == 3 && bits == 32) || (format == 1 && bits == 16)) || le16(fmt.data() + 12) != 2 * bytes || data_size % (2 * bytes) != 0) {
            std::fprintf(stderr, "IQ WAV requires stereo float32 or PCM16, I left / Q right\n");
            return 1;
        }
        rate = le32(fmt.data() + 4);
        dtype = format == 3 ? DataType::ComplexFloat32 : DataType::ComplexInt16;
        total_samples = data_size / (2 * bytes);
        data_path = wav_in;
        capture_center = std::stod(arg(argc, argv, "--capture-center", "0"));   // 原本と同じく録音時の中心は手で指定する
    } else if (const char* raw = arg(argc, argv, "--raw-sc16", nullptr)) {
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
    const double seconds_limit = std::stod(arg(argc, argv, "--seconds", "0"));

    ReceiverConfig cfg;
    cfg.in_rate = rate;
    cfg.rotate_hz = band_center - capture_center;
    cfg.band_center_hz = band_center;
    cfg.threads = std::stoi(arg(argc, argv, "--threads", "0"));
    for (const auto& c : channels) cfg.channels.push_back(channel_config(c));
    std::unique_ptr<Receiver> rxp;
    try {
        rxp = std::make_unique<Receiver>(cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    Receiver& rx = *rxp;
    std::printf("preset %s  band center %.6f MHz  capture %.6f MHz (rotate %+.1f Hz)  rate %.0f -> work %.0f (/%zu)  threads %d\n", preset->id.c_str(), band_center / 1e6,
                capture_center / 1e6, cfg.rotate_hz, rate, rx.work_rate(), rx.decim(), rx.threads());

    std::unique_ptr<IqWavWriter> wav;
    if (const char* w = arg(argc, argv, "--export-wav", nullptr)) {
        wav = std::make_unique<IqWavWriter>(w, static_cast<uint32_t>(std::lround(rx.work_rate())));
        if (!wav->ok()) {
            std::fprintf(stderr, "cannot write %s\n", w);
            return 1;
        }
    }
    const char* frames_dir = arg(argc, argv, "--frames-dir", nullptr);
    std::vector<FrameStream> streams(cfg.channels.size());
    if (frames_dir) std::filesystem::create_directories(frames_dir);
    // --decode-out(原本 decode-wave と同じ形式)
    const char* decode_dir = arg(argc, argv, "--decode-out", nullptr);
    std::vector<DecodeOut> outs(cfg.channels.size());
    if (decode_dir) {
        if (std::filesystem::exists(decode_dir)) {
            std::fprintf(stderr, "output directory must be new: %s\n", decode_dir);
            return 1;
        }
        std::filesystem::create_directories(decode_dir);
        std::ofstream(std::filesystem::path(decode_dir) / "INCOMPLETE") << "Decoding in progress; outputs may be incomplete.\n";
        for (std::size_t c = 0; c < cfg.channels.size(); ++c) {
            const auto dir = std::filesystem::path(decode_dir) / cfg.channels[c].id;
            std::filesystem::create_directory(dir);
            outs[c].wave = std::make_unique<PcmWave>(dir / "audio.wav");
            outs[c].data.open(dir / "data.jsonl");
        }
    }
    // --save(GUI の保存と同じ形式)
    std::unique_ptr<Recorder> recorder;
    if (const char* save = arg(argc, argv, "--save", nullptr)) {
        recorder = std::make_unique<Recorder>();
        std::filesystem::create_directories(save);
        try {
            recorder->start(std::filesystem::absolute(save), cfg.channels, OutputOptions{true, true, true});
        } catch (const std::exception& e) {
            std::fprintf(stderr, "save: %s\n", e.what());
            return 1;
        }
    }

    std::vector<uint64_t> message_counts(cfg.channels.size(), 0);
    Observer o;
    o.band = [&](std::span<const cf32> iq, uint64_t) {
        if (wav) wav->write(iq);
    };
    o.frame = [&](int ch, const FrameEvent& ev, const FrameTiming& t) {
        const auto c = static_cast<std::size_t>(ch);
        if (recorder) recorder->push_frame(cfg.channels[c].id, ev);
        if (frames_dir) {
            auto& s = streams[c];
            const bool cont = s.open && ev.epoch == s.epoch && ev.frame.bit_offset > s.last && (ev.frame.bit_offset - s.origin) % protocol::kFrameBits == 0;
            if (!cont) {
                s.bin.close();
                s.idx.close();
                s.smp.close();
                ++s.segment;
                char name[64];
                std::snprintf(name, sizeof name, "%sHz-%02d", cfg.channels[c].id.c_str(), s.segment);
                const auto dir = std::filesystem::path(frames_dir) / name;
                std::filesystem::create_directories(dir);
                s.bin.open(dir / "frames.bin", std::ios::binary);
                s.idx.open(dir / "indices.txt");
                s.smp.open(dir / "samples.txt");
                std::ofstream(dir / "format.txt") << protocol::to_string(cfg.channels[c].format) << "\n";
                s.epoch = ev.epoch;
                s.origin = ev.frame.bit_offset;
                s.open = true;
            }
            s.last = ev.frame.bit_offset;
            s.bin.write(reinterpret_cast<const char*>(ev.frame.received.data()), static_cast<std::streamsize>(ev.frame.received.size()));
            s.idx << (ev.frame.bit_offset - s.origin) / protocol::kFrameBits << "\n";
            // sample index と音声の誤り訂正の結果(訂正したビット数。音声フレームとして通らなければ -1)
            s.smp << t.input_sample_index << " " << (ev.corrected_bits ? static_cast<int>(*ev.corrected_bits) : -1) << "\n";
        }
        message_counts[c] += ev.messages.size();
        if (quiet) return;
        for (const auto& m : ev.messages)
            std::printf("MSG %-8s t=%.3fs in=%llu %s\n", channels[c].short_label.c_str(), static_cast<double>(t.time_us) / 1e6,
                        static_cast<unsigned long long>(t.input_sample_index), protocol::to_json(m).c_str());
    };
    o.audio = [&](int ch, const Audio& a) {
        if (recorder) recorder->push(a);
        if (decode_dir) outs[static_cast<std::size_t>(ch)].audio(a);
    };
    rx.set_observer(o);

    std::ifstream in(data_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", data_path.c_str());
        return 1;
    }
    in.seekg(static_cast<std::streamoff>(data_offset));
    const std::size_t block = 65536;
    std::vector<sc16> raw16(block);
    std::vector<cf32> iq(block);
    uint64_t idx = 0;
    const uint64_t limit = std::min(total_samples, seconds_limit > 0 ? static_cast<uint64_t>(seconds_limit * rate) : UINT64_MAX);
    std::size_t next_break = 0;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        while (idx < limit) {
            std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(block, limit - idx));
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
            if (decode_dir)
                for (int c = 0; c < rx.num_channels(); ++c)
                    for (const auto& m : rx.take_messages(c)) outs[static_cast<std::size_t>(c)].data << protocol::to_json(m) << "\n";
        }
        rx.flush();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (wav) {
        wav->close();
        if (wav->bytes() > 0xffffffffull - 36) std::fprintf(stderr, "warning: WAV exceeds 4 GiB; the header is clamped (split the recording)\n");
    }

    std::printf("\n%-8s %-11s %-6s %6s %8s %7s %7s %7s %6s %6s %6s %5s %s\n", "ch", "freq[MHz]", "type", "carrier", "offset", "EVM%", "frames", "synced", "voice", "audio",
                "data", "msgs", "epochs");
    std::string report_channels;
    for (int c = 0; c < rx.num_channels(); ++c) {
        const auto& def = channels[static_cast<std::size_t>(c)];
        const auto& st = rx.snapshot(c);
        std::printf("%-8s %-11.5f %-6s %6s %+8.1f %7.2f %7llu %7llu %6llu %6llu %6llu %5llu %llu%s\n", def.short_label.c_str(), def.frequency_hz / 1e6, to_string(def.service),
                    st.carrier_locked ? "LOCK" : "-", st.frequency_error_hz, st.evm_percent, static_cast<unsigned long long>(st.frames),
                    static_cast<unsigned long long>(st.synchronized_frames), static_cast<unsigned long long>(st.voice_frames), static_cast<unsigned long long>(st.audio_frames),
                    static_cast<unsigned long long>(st.data.valid_frames), static_cast<unsigned long long>(message_counts[static_cast<std::size_t>(c)]),
                    static_cast<unsigned long long>(st.epoch + 1), st.frame_locked ? "  FRAME" : "");
        if (st.operation_monitor) {
            for (const auto& r : st.operation_monitor->summary.rows)
                std::printf("         運行把握 %s 遅延 %s 在線 %s 最大 %s 分 %s\n", r.line.c_str(), r.delayed ? std::to_string(*r.delayed).c_str() : "?",
                            r.on_line ? std::to_string(*r.on_line).c_str() : "?", r.max_delay_min ? std::to_string(*r.max_delay_min).c_str() : "?",
                            r.train_no.value_or("").c_str());
        }
        if (decode_dir) {
            auto& out = outs[static_cast<std::size_t>(c)];
            out.wave->finish();
            out.data.close();
            std::ofstream mj(std::filesystem::path(decode_dir) / cfg.channels[static_cast<std::size_t>(c)].id / "messages.json");
            mj << "[";
            for (std::size_t i = 0; i < st.messages.size(); ++i) mj << (i ? ",\n" : "\n") << "{\"message\":" << protocol::to_json(st.messages[i].message) << ",\"receptions\":" << st.messages[i].receptions << "}";
            mj << "\n]\n";
            if (!report_channels.empty()) report_channels += ",";
            report_channels += "{\"channel\":{\"id\":" + protocol::json_string(cfg.channels[static_cast<std::size_t>(c)].id) + ",\"label\":" +
                               protocol::json_string(cfg.channels[static_cast<std::size_t>(c)].label) + "},\"status\":" + status_json(st) + "}";
        }
    }
    const double seconds = static_cast<double>(idx) / rate;
    if (decode_dir) {
        std::ofstream(std::filesystem::path(decode_dir) / "report.json") << "{\"sample_rate\":" << rate << ",\"center_hz\":" << band_center << ",\"samples\":" << idx
                                                                         << ",\"input_seconds\":" << seconds << ",\"processing_seconds\":" << elapsed << ",\"channels\":["
                                                                         << report_channels << "]}\n";
        std::filesystem::remove(std::filesystem::path(decode_dir) / "INCOMPLETE");
    }
    if (recorder) {
        const auto s = recorder->stop();
        std::printf("saved %s: %llu WAV, %llu samples, %llu raw frames, %llu messages%s%s\n", s.directory.value_or("").c_str(), static_cast<unsigned long long>(s.files),
                    static_cast<unsigned long long>(s.pcm_samples), static_cast<unsigned long long>(s.raw_frames), static_cast<unsigned long long>(s.data_messages),
                    s.error ? "  ERROR " : "", s.error.value_or("").c_str());
    }
    std::printf("samples %llu (%.2f s), processed in %.2f s (%.0f%% of real time)\n", static_cast<unsigned long long>(idx), seconds, elapsed, seconds > 0 ? elapsed / seconds * 100 : 0);
    if (wav) std::printf("wrote %s (%.0f S/s stereo float32, center %.0f Hz)\n", arg(argc, argv, "--export-wav", ""), rx.work_rate(), band_center);
    return 0;
}
