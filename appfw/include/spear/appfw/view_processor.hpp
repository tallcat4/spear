// spear-gui — View processor (要件 §9.2)
//
// IQ → FFT / view processor → lightweight view frame → QML。
// Stream Bus の LatestOnly consumer として自前 thread で動く。GUI thread でも DSP thread でもない。
// GUI が遅ければ自分の stream が drop するだけで、他 consumer に波及しない (§10)。
#pragma once

#include "spear/core/stream.hpp"
#include "spear/dsp/spectrum.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace spear::appfw {

struct SpectrumFrame {
    uint64_t seq = 0;            // frame 通し番号(0 = 無し)
    std::vector<float> db;       // nfft 点、fftshift 済み [dBFS]
    std::vector<float> max_hold; // nfft 点
    double sample_rate = 0, center_freq = 0;
    float peak_db = -999; double peak_offset_hz = 0;
    float noise_floor_db = -999; // median 推定
    uint64_t sample_index = 0;   // 元 block の先頭 index(provenance)
    uint64_t generation = 0;
    Flags flags;                 // 元 block の flags(discontinuity 等を画面に出す)
};

class ViewProcessor {
public:
    // stream は sc16 または cf32 の complex stream(radio.rx でも App 内部の channel IQ でも同じ)
    ViewProcessor(StreamBase& stream, std::string consumer_name, std::size_t nfft = 1024,
                  std::size_t waterfall_rows = 1024, double max_fps = 30.0);
    ~ViewProcessor();

    void start();
    void stop();

    // GUI thread から。seq が変わっていれば out を更新して true。
    bool latest(SpectrumFrame& out, uint64_t last_seen_seq) const;

    // waterfall 行: RGBA8 (width = nfft)。rows_written() は累計。
    std::size_t width() const { return nfft_; }
    std::size_t rows() const { return rows_; }
    uint64_t rows_written() const { return rows_written_.load(); }
    // [from, rows_written) の行を out へ(古い順)。ring を超えた分は最新 rows_ 行に切り詰める。戻り値 = 最初の行番号。
    uint64_t copy_rows(uint64_t from, std::vector<uint32_t>& out) const;

    void set_db_range(float min_db, float max_db);
    void get_db_range(float& min_db, float& max_db) const { min_db = db_min_; max_db = db_max_; }
    void set_averaging(int frames) { avg_frames_ = frames < 1 ? 1 : frames; }
    void reset_max_hold() { reset_max_ = true; }
    void request_auto_range() { auto_range_ = true; }   // 次の frame でノイズ床から dB レンジを決める

    ConsumerStats stats() const { return sub_ ? sub_->stats() : ConsumerStats{}; }

private:
    void run();
    uint32_t colorize(float db) const;

    StreamBase& stream_;
    std::string name_;
    std::size_t nfft_, rows_;
    double max_fps_;
    std::shared_ptr<Subscription> sub_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<int> avg_frames_{4};
    std::atomic<bool> reset_max_{false};
    std::atomic<bool> auto_range_{true};
    std::atomic<float> db_min_{-110.f}, db_max_{-20.f};

    mutable std::mutex mu_;
    SpectrumFrame frame_;
    std::vector<uint32_t> ring_;          // rows_ * nfft_
    std::atomic<uint64_t> rows_written_{0};
    std::vector<uint32_t> palette_;       // 256 entries
};

} // namespace spear::appfw
