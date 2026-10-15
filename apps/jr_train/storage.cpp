// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/receiver-core/src/recording.rs と wave.rs
#include "storage.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace spear::jr_train {
namespace {

constexpr std::size_t kQueueFrames = 2048;
constexpr uint64_t kSegmentSamples = 8000ull * 60 * 30;
constexpr uint64_t kGapSamples = 8000ull * 5;

// ---- 最小の JSON(原本は serde_json::json!: オブジェクトのキーは辞書順)----
struct Json {
    enum Kind { Null, Bool, Int, Real, Str, Raw, Arr, Obj } kind = Null;
    bool b = false;
    int64_t i = 0;
    double d = 0;
    std::string s;   // Str / Raw(整形済みの JSON)
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : kind(Bool), b(v) {}
    Json(int v) : kind(Int), i(v) {}
    Json(uint32_t v) : kind(Int), i(v) {}
    Json(uint64_t v) : kind(Int), i(static_cast<int64_t>(v)) {}
    Json(double v) : kind(Real), d(v) {}
    Json(const char* v) : kind(Str), s(v) {}
    Json(std::string v) : kind(Str), s(std::move(v)) {}
    template <class T>
    Json(const std::optional<T>& v) {
        if (v) *this = Json(*v);
    }
    static Json raw(std::string v) {
        Json j;
        j.kind = Raw;
        j.s = std::move(v);
        return j;
    }
    static Json object() {
        Json j;
        j.kind = Obj;
        return j;
    }
    static Json array() {
        Json j;
        j.kind = Arr;
        return j;
    }
    Json& operator[](const std::string& k) {
        kind = Obj;
        return obj[k];
    }
    std::string dump(int indent = -1, int level = 0) const {
        const std::string nl = indent >= 0 ? "\n" : "", pad = indent >= 0 ? std::string(static_cast<std::size_t>((level + 1) * indent), ' ') : "",
                          end = indent >= 0 ? std::string(static_cast<std::size_t>(level * indent), ' ') : "";
        switch (kind) {
        case Null: return "null";
        case Bool: return b ? "true" : "false";
        case Int: return std::to_string(i);
        case Real: {
            char buf[40];
            if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 1e16) std::snprintf(buf, sizeof buf, "%.1f", d);
            else std::snprintf(buf, sizeof buf, "%.17g", d);
            return buf;
        }
        case Str: return protocol::json_string(s);
        case Raw: return s;
        case Arr: {
            if (arr.empty()) return "[]";
            std::string out = "[" + nl;
            for (std::size_t k = 0; k < arr.size(); ++k) out += pad + arr[k].dump(indent, level + 1) + (k + 1 < arr.size() ? "," : "") + nl;
            return out + end + "]";
        }
        case Obj: {
            if (obj.empty()) return "{}";
            std::string out = "{" + nl;
            std::size_t k = 0;
            for (const auto& [key, v] : obj) out += pad + protocol::json_string(key) + (indent >= 0 ? ": " : ":") + v.dump(indent, level + 1) + (++k < obj.size() ? "," : "") + nl;
            return out + end + "}";
        }
        }
        return "null";
    }
};

std::string utc_now_rfc3339() { return now_rfc3339(); }

std::string stem_now() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%S", &tm);
    char out[96];
    std::snprintf(out, sizeof out, "recording-%s.%03dZ", buf, static_cast<int>(ms % 1000));
    return out;
}

Json options_json(const OutputOptions& o) {
    Json j = Json::object();
    j["audio"] = o.audio;
    j["raw_frames"] = o.raw_frames;
    j["data_messages"] = o.data_messages;
    return j;
}

Json channel_json(const ChannelConfig& c) {
    Json j = Json::object();
    j["id"] = c.id;
    j["label"] = c.label;
    j["frequency_hz"] = c.frequency_hz;
    j["format"] = protocol::to_string(c.format);
    return j;
}

// 作成だけ(既存なら失敗)して整形済み JSON を書き、同期する(原本 create_json)
void create_json(const std::filesystem::path& path, const Json& value) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) throw std::runtime_error(path.filename().string() + " を作成できません");
    const std::string text = value.dump(2) + "\n";
    const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) throw std::runtime_error(path.filename().string() + " を書き込めません");
}

std::FILE* create_new(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return nullptr;
    std::FILE* f = ::fdopen(fd, "w+b");
    if (!f) ::close(fd);
    return f;
}

struct Clip {
    std::unique_ptr<PcmWave> wave;
    std::string name;
    uint64_t epoch = 0;
    uint32_t first_index = 0, last_index = 0;
    uint64_t samples = 0;
};
struct Track {
    ChannelConfig config;
    uint64_t number = 0;
    std::optional<Clip> clip;
    std::optional<std::pair<uint64_t, uint32_t>> last;
};

class Writer {
public:
    std::filesystem::path directory;
    std::string started_at;
    uint64_t files = 0, pcm_samples = 0, raw_frames = 0, data_messages = 0;

    Writer(const std::filesystem::path& parent, const std::vector<ChannelConfig>& channels, OutputOptions options) {
        std::error_code ec;
        if (!parent.is_absolute() || !std::filesystem::is_directory(parent, ec)) throw std::runtime_error("保存先には存在するフォルダーの絶対パスを指定してください。");
        if (channels.empty() || channels.size() > 64) throw std::runtime_error("保存対象は 1–64 波にしてください。");
        std::set<uint64_t> frequencies;
        for (const auto& c : channels) {
            if (c.id.empty() || !std::isfinite(c.frequency_hz) || c.frequency_hz <= 0 || c.frequency_hz > 100e9 || c.frequency_hz != std::floor(c.frequency_hz) ||
                !frequencies.insert(static_cast<uint64_t>(c.frequency_hz)).second || tracks_.count(c.id))
                throw std::runtime_error("保存対象の ID・周波数が不正または重複しています。");
            tracks_[c.id] = Track{c, 0, std::nullopt, std::nullopt};
        }
        const std::string stem = stem_now();
        for (int n = 0; n < 1000 && directory.empty(); ++n) {
            char suffix[8];
            std::snprintf(suffix, sizeof suffix, "-%03d", n);
            const auto candidate = parent / (stem + suffix);
            if (std::filesystem::create_directory(candidate, ec)) directory = candidate;
            else if (ec) throw std::runtime_error("保存フォルダーを作成できません:" + ec.message());
        }
        if (directory.empty()) throw std::runtime_error("保存フォルダー名が重複しました。別の保存先を指定してください。");
        // 異常終了しても保存中だったことが分かるように
        Json incomplete = Json::object();
        incomplete["message"] = "Recording is in progress or incomplete. WAV headers are finalized on stop.";
        create_json(directory / "INCOMPLETE", incomplete);
        started_at = utc_now_rfc3339();
        Json session = Json::object();
        session["version"] = 2;
        session["outputs"] = options_json(options);
        session["started_at"] = started_at;
        session["sample_rate"] = 8000;
        session["bits_per_sample"] = 16;
        session["audio_channels"] = 1;
        session["raw_frame_encoding"] = "Unchanged 48-byte radio envelope, hexadecimal in received byte order (MSB-first packed bits).";
        session["received_at"] = "Host UTC time at decoding; not the transmitter timestamp. JSONL is ordered within each channel, not globally by RF time.";
        Json chans = Json::array();
        for (const auto& c : channels) chans.arr.push_back(channel_json(c));
        session["channels"] = chans;
        session["segment_max_seconds"] = 1800;
        session["gap_max_seconds"] = 5;
        session["timeline"] = "Each file starts at its first decoded PCM frame; long gaps and resynchronization create a new file. Frame indices are local to each channel epoch.";
        session["receiver"] = "S.P.E.A.R. JR TRAIN RADIO (C++ port of jr-conventional-line-radio-receiver)";
        create_json(directory / "session.json", session);
        index_ = create_new(directory / "files.jsonl");
        if (!index_) throw std::runtime_error("録音索引を作成できません");
        if (options.raw_frames && !(raw_ = create_new(directory / "raw-frames.jsonl"))) throw std::runtime_error("raw-frames.jsonl を作成できません");
        if (options.data_messages && !(messages_ = create_new(directory / "data-messages.jsonl"))) throw std::runtime_error("data-messages.jsonl を作成できません");
        last_flush_ = std::chrono::steady_clock::now();
    }
    ~Writer() {
        for (std::FILE* f : {index_, raw_, messages_})
            if (f) std::fclose(f);
    }

    void event(Recorder::Event& e) {
        if (auto* a = std::get_if<Audio>(&e)) {
            audio(*a);
        } else {
            auto& fi = std::get<Recorder::FrameItem>(e);
            auto it = tracks_.find(fi.channel);
            if (it == tracks_.end()) throw std::runtime_error("保存対象外のフレームを受信しました。");
            const auto& cfg = it->second.config;
            const auto& ev = fi.event;
            Json base = Json::object();
            base["schema_version"] = 1;
            base["received_at"] = fi.received_at;
            base["channel_id"] = fi.channel;
            base["frequency_hz"] = cfg.frequency_hz;
            base["label"] = cfg.label;
            base["format"] = protocol::to_string(cfg.format);
            base["epoch"] = ev.epoch;
            base["index"] = ev.index;
            base["bit_offset"] = ev.frame.bit_offset;
            if (raw_) {
                Json row = base;
                row["raw_hex"] = protocol::hex_upper(ev.frame.received.data(), ev.frame.received.size());
                row["sync_matched"] = ev.frame.sync_matched;
                row["marker_errors"] = ev.frame.marker_errors;
                row["voice_accepted"] = ev.corrected_bits.has_value();
                row["corrected_bits"] = ev.corrected_bits ? Json(static_cast<uint32_t>(*ev.corrected_bits)) : Json(nullptr);
                line(raw_, row);
                ++raw_frames;
            }
            if (messages_) {
                for (const auto& m : ev.messages) {
                    Json row = base;
                    row["message"] = Json::raw(protocol::to_json_sorted(m));
                    line(messages_, row);
                    ++data_messages;
                }
            }
        }
        if (std::chrono::steady_clock::now() - last_flush_ >= std::chrono::milliseconds(500)) flush_jsonl();
    }

    void flush_jsonl() {
        for (std::FILE* f : {raw_, messages_})
            if (f && std::fflush(f) != 0) throw std::runtime_error("JSONL を保存できません。空き容量を確認してください");
        last_flush_ = std::chrono::steady_clock::now();
    }

    // 戻り値: (ファイル数, PCM sample 数, エラー)
    std::optional<std::string> finish(std::optional<std::string> error) {
        for (auto& [id, track] : tracks_) {
            try {
                close(track);
            } catch (const std::exception& e) {
                if (!error) error = e.what();
            }
        }
        if (index_ && (std::fflush(index_) != 0 || ::fsync(fileno(index_)) != 0) && !error) error = "files.jsonl を確定できません";
        for (std::FILE* f : {raw_, messages_})
            if (f && (std::fflush(f) != 0 || ::fsync(fileno(f)) != 0) && !error) error = "JSONL を確定できません";
        Json result = Json::object();
        result["raw_frames"] = raw_frames;
        result["data_messages"] = data_messages;
        result["finished_at"] = utc_now_rfc3339();
        result["files"] = files;
        result["pcm_samples"] = pcm_samples;
        result["error"] = error;
        try {
            create_json(directory / "result.json", result);
        } catch (const std::exception& e) {
            if (!error) error = e.what();
        }
        if (!error) {
            std::error_code ec;
            if (!std::filesystem::remove(directory / "INCOMPLETE", ec)) error = "INCOMPLETE を削除できません";
        }
        return error;
    }

private:
    static void line(std::FILE* f, const Json& j) {
        const std::string s = j.dump() + "\n";
        if (std::fwrite(s.data(), 1, s.size(), f) != s.size()) throw std::runtime_error("JSONL を書き込めません");
    }
    void close(Track& track) {
        if (!track.clip) return;
        auto clip = std::move(*track.clip);
        track.clip.reset();
        const uint64_t samples = clip.wave->samples();
        clip.wave->finish();
        Json row = Json::object();
        row["file"] = clip.name;
        row["channel_id"] = track.config.id;
        row["frequency_hz"] = track.config.frequency_hz;
        row["label"] = track.config.label;
        row["epoch"] = clip.epoch;
        row["first_index"] = clip.first_index;
        row["last_index"] = clip.last_index;
        row["samples"] = samples;
        line(index_, row);
    }
    void audio(const Audio& a) {
        auto it = tracks_.find(a.channel);
        if (it == tracks_.end()) throw std::runtime_error("保存対象外の音声を受信しました。");
        Track& track = it->second;
        if (track.last && (a.epoch < track.last->first || (a.epoch == track.last->first && a.index <= track.last->second)))
            throw std::runtime_error("音声フレームの順序が不正なため録音を終了しました。");
        const uint64_t gap = track.last && track.last->first == a.epoch ? static_cast<uint64_t>(a.index - track.last->second - 1) * 320 : 0;
        const bool split = track.clip && (track.clip->epoch != a.epoch || gap > kGapSamples || track.clip->samples + gap + 320 > kSegmentSamples);
        if (split) close(track);
        if (!track.clip) {
            ++track.number;
            char name[64];
            std::snprintf(name, sizeof name, "%lluHz-%06llu.wav", static_cast<unsigned long long>(track.config.frequency_hz), static_cast<unsigned long long>(track.number));
            Clip c;
            c.wave = std::make_unique<PcmWave>(directory / name);
            c.name = name;
            c.epoch = a.epoch;
            c.first_index = c.last_index = a.index;
            track.clip = std::move(c);
            ++files;
        }
        Clip& clip = *track.clip;
        if (clip.samples > 0 && gap > 0) {
            clip.wave->silence(gap);
            clip.samples += gap;
        }
        clip.wave->write(a.pcm.data(), a.pcm.size());
        clip.samples += 320;
        clip.last_index = a.index;
        track.last = std::make_pair(a.epoch, a.index);
        pcm_samples += 320;
    }

    std::map<std::string, Track> tracks_;
    std::FILE* index_ = nullptr;
    std::FILE* raw_ = nullptr;
    std::FILE* messages_ = nullptr;
    std::chrono::steady_clock::time_point last_flush_;
};

} // namespace

// ---- PcmWave ----

PcmWave::PcmWave(const std::filesystem::path& path) : path_(path) {
    f_ = create_new(path);
    if (!f_) throw std::runtime_error(path.filename().string() + " を作成できません");
    header();
}

void PcmWave::write(const int16_t* pcm, std::size_t n) {
    if ((samples_ + n) * 2 > 0xffffffffull - 36) throw std::runtime_error("WAV size limit reached");
    uint8_t buf[2 * 8000];
    std::size_t done = 0;
    while (done < n) {
        const std::size_t k = std::min<std::size_t>(8000, n - done);
        for (std::size_t i = 0; i < k; ++i) {
            const auto v = static_cast<uint16_t>(pcm[done + i]);
            buf[2 * i] = static_cast<uint8_t>(v & 0xff);
            buf[2 * i + 1] = static_cast<uint8_t>(v >> 8);
        }
        if (std::fwrite(buf, 1, 2 * k, f_) != 2 * k) throw std::runtime_error("音声を書き込めません。保存先の空き容量を確認してください");
        done += k;
    }
    samples_ += n;
}

void PcmWave::silence(uint64_t samples) {
    if (samples > (0xffffffffull - 36) / 2 - samples_) throw std::runtime_error("WAV gap exceeds RIFF size limit");
    static const int16_t zero[8000] = {};
    while (samples > 0) {
        const auto n = std::min<uint64_t>(samples, 8000);
        write(zero, static_cast<std::size_t>(n));
        samples -= n;
    }
}

void PcmWave::header() {
    const auto size = static_cast<uint32_t>(samples_ * 2);
    auto le32 = [](uint8_t* p, uint32_t v) {
        for (int k = 0; k < 4; ++k) p[k] = static_cast<uint8_t>(v >> (8 * k));
    };
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    le32(h + 4, 36 + size);
    le32(h + 24, 8000);
    le32(h + 28, 16000);
    le32(h + 40, size);
    std::fseek(f_, 0, SEEK_SET);
    if (std::fwrite(h, 1, 44, f_) != 44) throw std::runtime_error("WAV ヘッダを書き込めません");
    std::fseek(f_, 0, SEEK_END);
}

void PcmWave::finish() {
    if (!f_) return;
    std::fflush(f_);
    // 書き込みに失敗したブロックの一部は確定した RIFF に含めない
    if (::ftruncate(fileno(f_), static_cast<off_t>(44 + samples_ * 2)) != 0) throw std::runtime_error(path_.filename().string() + " の WAV を確定できません");
    header();
    const bool ok = std::fflush(f_) == 0 && ::fsync(fileno(f_)) == 0;
    std::fclose(f_);
    f_ = nullptr;
    if (!ok) throw std::runtime_error(path_.filename().string() + " の WAV を確定できません");
}

// ---- Recorder ----

Recorder::~Recorder() { stop(); }

RecordingStatus Recorder::snapshot() const {
    std::lock_guard<std::mutex> lk(status_mu_);
    return status_;
}

RecordingStatus Recorder::start(const std::filesystem::path& parent, const std::vector<ChannelConfig>& channels, OutputOptions options) {
    if (!options.audio && !options.raw_frames && !options.data_messages) throw std::runtime_error("保存する内容を 1 つ以上選択してください。");
    std::lock_guard<std::mutex> op(op_mu_);
    {
        const auto s = snapshot();
        if (s.state == "recording" || s.state == "stopping") throw std::runtime_error("すでに保存中です。保存を停止してから開始してください。");
    }
    if (thread_.joinable()) thread_.join();
    auto writer = std::make_shared<Writer>(parent, channels, options);
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        status_ = RecordingStatus{};
        status_.state = "recording";
        status_.options = options;
        status_.directory = writer->directory.string();
        status_.started_at = writer->started_at;
    }
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        queue_.clear();
        closed_ = false;
        sending_ = true;
        options_ = options;
    }
    thread_ = std::thread([this, writer] {
        std::optional<std::string> error;
        try {
            for (;;) {
                std::unique_lock<std::mutex> lk(q_mu_);
                const bool got = q_cv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return !queue_.empty() || closed_; });
                if (!got) {
                    lk.unlock();
                    writer->flush_jsonl();
                    continue;
                }
                if (queue_.empty() && closed_) break;
                Event e = std::move(queue_.front());
                queue_.pop_front();
                lk.unlock();
                writer->event(e);
                std::lock_guard<std::mutex> sl(status_mu_);
                status_.files = writer->files;
                status_.pcm_samples = writer->pcm_samples;
                status_.raw_frames = writer->raw_frames;
                status_.data_messages = writer->data_messages;
            }
        } catch (const std::exception& e) {
            error = e.what();
            std::lock_guard<std::mutex> lk(q_mu_);
            sending_ = false;
            queue_.clear();
        }
        std::optional<std::string> previous;
        {
            std::lock_guard<std::mutex> sl(status_mu_);
            previous = status_.error;
        }
        const auto final_error = writer->finish(error ? error : previous);
        std::lock_guard<std::mutex> sl(status_mu_);
        status_.files = writer->files;
        status_.pcm_samples = writer->pcm_samples;
        status_.raw_frames = writer->raw_frames;
        status_.data_messages = writer->data_messages;
        if (final_error) status_.error = final_error;
        status_.state = status_.error ? "failed" : "completed";
        status_.finished_at = utc_now_rfc3339();
    });
    return snapshot();
}

void Recorder::send(Event e) {
    std::lock_guard<std::mutex> lk(q_mu_);
    if (!sending_) return;
    if (queue_.size() >= kQueueFrames) {
        // 書き込みが追いつかない: 保存を終える(受信は止めない)
        sending_ = false;
        closed_ = true;
        q_cv_.notify_one();
        std::lock_guard<std::mutex> sl(status_mu_);
        if (!status_.error) status_.error = "保存処理が追いつかず保存を終了しました。保存先の速度と空き容量を確認してください。";
        if (status_.state == "recording") status_.state = "stopping";
        return;
    }
    queue_.push_back(std::move(e));
    q_cv_.notify_one();
}

void Recorder::push(const Audio& audio) {
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        if (!sending_ || !options_.audio) return;
    }
    send(audio);
}

void Recorder::push_frame(const std::string& channel, const FrameEvent& event) {
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        if (!sending_ || !(options_.raw_frames || (options_.data_messages && !event.messages.empty()))) return;
    }
    send(FrameItem{channel, utc_now_rfc3339(), event});
}

RecordingStatus Recorder::stop() {
    std::lock_guard<std::mutex> op(op_mu_);
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        sending_ = false;
        closed_ = true;
        q_cv_.notify_one();
    }
    {
        std::lock_guard<std::mutex> sl(status_mu_);
        if (status_.state == "recording") status_.state = "stopping";
    }
    if (thread_.joinable()) thread_.join();
    return snapshot();
}

} // namespace spear::jr_train
