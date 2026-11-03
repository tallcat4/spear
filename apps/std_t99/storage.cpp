#include "storage.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <stdexcept>

namespace spear::std_t99 {

namespace {
void put_u32(std::ofstream& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.put(static_cast<char>((v >> (8 * i)) & 0xff)); }
void put_u16(std::ofstream& o, uint16_t v) { o.put(static_cast<char>(v & 0xff)); o.put(static_cast<char>(v >> 8)); }

std::ofstream create_new(const std::filesystem::path& p) {
    if (std::filesystem::exists(p)) throw std::runtime_error("file exists: " + p.string());
    std::ofstream o(p, std::ios::binary);
    if (!o) throw std::runtime_error("cannot create " + p.string());
    return o;
}
}

std::string Recorder::start(const std::filesystem::path& dir, int channel) {
    stop();
    std::filesystem::create_directories(dir);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char name[64];
    std::snprintf(name, sizeof name, "T99-%04d%02d%02d-%02d%02d%02d-CH%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, channel);
    const auto base = (dir / name).string();
    std::lock_guard<std::mutex> lk(mu_);
    burst_ = create_new(base + ".burst");
    llr_ = create_new(base + ".llr");
    jsonl_ = create_new(base + ".jsonl");
    wav_ = create_new(base + ".wav");
    stem_ = base;
    records_ = frames_ = samples_ = 0;
    write_wav_header();
    open_ = true;
    return stem_;
}

void Recorder::write_wav_header() {
    // 8 kHz / 16 bit / mono。長さは stop() で書き直す
    wav_.seekp(0);
    wav_.write("RIFF", 4);
    put_u32(wav_, static_cast<uint32_t>(36 + samples_ * 2));
    wav_.write("WAVEfmt ", 8);
    put_u32(wav_, 16);
    put_u16(wav_, 1);
    put_u16(wav_, 1);
    put_u32(wav_, 8000);
    put_u32(wav_, 16000);
    put_u16(wav_, 2);
    put_u16(wav_, 16);
    wav_.write("data", 4);
    put_u32(wav_, static_cast<uint32_t>(samples_ * 2));
}

void Recorder::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open_) return;
    write_wav_header();
    burst_.close(); llr_.close(); jsonl_.close(); wav_.close();
    open_ = false;
}

void Recorder::frame(const Frame& f, const std::string& json, const std::string& utc_iso) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open_) return;
    int64_t burst = -1;
    if (f.voice()) {
        burst_.put(static_cast<char>(0xff));
        for (int i = 0; i < 4; ++i) {
            burst_.put(0x48);
            const auto& h = f.halves[static_cast<std::size_t>(i / 2)];
            burst_.write(reinterpret_cast<const char*>(h.payload.data() + 9 * (i % 2)), 9);
        }
        for (const auto& h : f.halves) llr_.write(reinterpret_cast<const char*>(h.llr.data()), static_cast<std::streamsize>(h.llr.size()));
        burst = static_cast<int64_t>(records_++);
    }
    std::string line = json;
    const std::string none = ",\"burst\":null,";
    if (const auto at = line.find(none); at != std::string::npos && burst >= 0)
        line.replace(at, none.size(), ",\"burst\":" + std::to_string(burst) + ",");
    // {"utc":"...", + 元の中身
    if (!line.empty() && line.front() == '{') line = "{\"utc\":\"" + utc_iso + "\"," + line.substr(1);
    jsonl_ << line << '\n';
    ++frames_;
}

void Recorder::audio(std::span<const float> pcm) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open_) return;
    for (float v : pcm) {
        const auto s = static_cast<int16_t>(std::clamp(std::lround(v * 32767.f), -32768L, 32767L));
        put_u16(wav_, static_cast<uint16_t>(s));
    }
    samples_ += pcm.size();
}

} // namespace spear::std_t99
