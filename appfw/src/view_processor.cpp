#include "spear/appfw/view_processor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace spear::appfw {

using namespace std::chrono_literals;

namespace {
// 計測器らしい抑制された palette: 黒 → 濃青 → 青 → 水色 → 緑 → 黄 → 白(発光・グラデーション装飾ではなく信号強度の符号化)
// 入力 stops は 0xRRGGBB。出力は RGBA8 テクスチャのメモリ順(R,G,B,A)= little-endian の uint32 では 0xAABBGGRR。
uint32_t lerp_rgb(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int sh) { return static_cast<uint32_t>(std::lround(((a >> sh) & 0xff) * (1 - t) + ((b >> sh) & 0xff) * t)); };
    const uint32_t r = ch(16), g = ch(8), bl = ch(0);
    return 0xff000000u | (bl << 16) | (g << 8) | r;
}
} // namespace

ViewProcessor::ViewProcessor(StreamBase& stream, std::string consumer_name, std::size_t nfft,
                             std::size_t waterfall_rows, double max_fps)
    : stream_(stream), name_(std::move(consumer_name)), nfft_(nfft), rows_(waterfall_rows), max_fps_(max_fps),
      ring_(nfft * waterfall_rows, 0xff000000u) {
    frame_.db.assign(nfft, -999.f);
    frame_.max_hold.assign(nfft, -999.f);
    // palette stops (0xRRGGBB)
    // 下半分(ノイズ床付近)は暗く、信号が浮き出るように。上端の白は飽和の目印。
    // auto range ではノイズ床が t≈0.15 に来る: そこが「見える暗紺」になるように置く。
    const std::vector<std::pair<float, uint32_t>> stops = {
        {0.00f, 0x000000}, {0.10f, 0x000818}, {0.22f, 0x001c4c}, {0.40f, 0x00408c}, {0.55f, 0x0080b8},
        {0.68f, 0x00c0c0}, {0.80f, 0x30d030}, {0.90f, 0xf0e020}, {1.00f, 0xffffff}};
    palette_.resize(256);
    for (int i = 0; i < 256; ++i) {
        const float t = i / 255.f;
        std::size_t k = 0;
        while (k + 1 < stops.size() && stops[k + 1].first < t) ++k;
        const float span = stops[k + 1].first - stops[k].first;
        palette_[i] = lerp_rgb(stops[k].second, stops[k + 1].second, span > 0 ? (t - stops[k].first) / span : 0);
    }
}

ViewProcessor::~ViewProcessor() { stop(); }

void ViewProcessor::start() {
    if (th_.joinable()) return;
    stop_ = false;
    sub_ = stream_.subscribe(name_, DeliveryPolicy::LatestOnly, 1);
    th_ = std::thread([this] { run(); });
}

void ViewProcessor::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    sub_.reset();
}

void ViewProcessor::set_db_range(float lo, float hi) {
    if (hi - lo < 10.f) hi = lo + 10.f;
    db_min_ = lo;
    db_max_ = hi;
}

uint32_t ViewProcessor::colorize(float db) const {
    const float lo = db_min_, hi = db_max_;
    float t = (db - lo) / (hi - lo);
    t = std::clamp(t, 0.f, 1.f);
    return palette_[static_cast<std::size_t>(t * 255.f)];
}

bool ViewProcessor::latest(SpectrumFrame& out, uint64_t last_seen) const {
    std::lock_guard lk(mu_);
    if (frame_.seq == last_seen) return false;
    out = frame_;
    return true;
}

uint64_t ViewProcessor::copy_rows(uint64_t from, std::vector<uint32_t>& out) const {
    std::lock_guard lk(mu_);
    const uint64_t end = rows_written_.load();
    uint64_t begin = from;
    if (end > rows_ && begin < end - rows_) begin = end - rows_;
    if (begin > end) begin = end;
    out.resize((end - begin) * nfft_);
    for (uint64_t r = begin; r < end; ++r)
        std::copy_n(ring_.data() + (r % rows_) * nfft_, nfft_, out.data() + (r - begin) * nfft_);
    return begin;
}

void ViewProcessor::run() {
    dsp::SpectrumEstimator est(nfft_);
    std::vector<float> db, acc(nfft_, 0.f), sorted;
    std::vector<float> maxh(nfft_, -999.f);
    int acc_n = 0;
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / max_fps_));
    auto next = std::chrono::steady_clock::now();
    // nfft より短いブロック(低レートの stream: 12.5 kHz のチャネル IQ は 16384 入力あたり 51 sample)は nfft ぶん溜めてから 1 フレームにする。
    // 溜めている間の header は最初のブロックのもの(sample_index = フレーム先頭)、flags は OR。generation が変わったら捨てて溜め直す
    std::vector<cf32> gather32; std::vector<sc16> gather16;
    BlockHeader gh; Flags gflags;
    while (!stop_) {
        auto d = sub_->pop(50ms);
        if (!d) continue;
        BlockHeader h = d->block.header();
        Flags flags = d->flags;
        bool ok = false;
        if (h.sample_count >= nfft_) {
            gather32.clear(); gather16.clear();
            switch (h.dtype) {
            case DataType::ComplexInt16:   ok = est.compute_averaged(d->block.as<sc16>(), db); break;
            case DataType::ComplexFloat32: ok = est.compute_averaged(d->block.as<cf32>(), db); break;
            default: break;
            }
        } else {
            const bool fresh = gather32.empty() && gather16.empty();
            if (!fresh && h.generation != gh.generation) { gather32.clear(); gather16.clear(); }
            if (gather32.empty() && gather16.empty()) { gh = h; gflags = flags; } else gflags |= flags;
            switch (h.dtype) {
            case DataType::ComplexInt16: { auto x = d->block.as<sc16>(); gather16.insert(gather16.end(), x.begin(), x.end()); break; }
            case DataType::ComplexFloat32: { auto x = d->block.as<cf32>(); gather32.insert(gather32.end(), x.begin(), x.end()); break; }
            default: continue;
            }
            if (gather16.size() >= nfft_) { ok = est.compute_averaged(std::span<const sc16>(gather16), db); gather16.clear(); }
            else if (gather32.size() >= nfft_) { ok = est.compute_averaged(std::span<const cf32>(gather32), db); gather32.clear(); }
            else continue;
            h = gh; flags = gflags;
        }
        if (!ok) continue;
        // 時間平均(power 領域ではなく dB 領域の移動平均で十分: 表示用)
        const int want = avg_frames_.load();
        if (acc_n == 0) std::fill(acc.begin(), acc.end(), 0.f);
        for (std::size_t i = 0; i < nfft_; ++i) acc[i] += db[i];
        ++acc_n;
        if (acc_n < want) continue;
        for (std::size_t i = 0; i < nfft_; ++i) db[i] = acc[i] / static_cast<float>(acc_n);
        acc_n = 0;
        // frame rate 上限(GUI は LatestOnly なので、ここで間引いても drop として数えられるだけ)
        if (std::chrono::steady_clock::now() < next) continue;
        next += period;
        if (next < std::chrono::steady_clock::now()) next = std::chrono::steady_clock::now();

        if (reset_max_.exchange(false)) std::fill(maxh.begin(), maxh.end(), -999.f);
        std::size_t pk = 0;
        for (std::size_t i = 0; i < nfft_; ++i) {
            maxh[i] = std::max(maxh[i], db[i]);
            if (db[i] > db[pk]) pk = i;
        }
        sorted = db;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(nfft_ / 2), sorted.end());
        if (auto_range_.exchange(false)) {
            // 一度だけ。ノイズ床(中央値)を下から 1.5 div に置くが、フィルタ済み stream(阻止域が床になる)では
            // ピーク基準(peak − 60 dB)を下限にする。以後は手動(REF キー)
            const float nf = std::floor(sorted[nfft_ / 2] / 10.f) * 10.f;
            float pk = -999.f;
            for (float v : db) pk = std::max(pk, v);
            const float lo = std::max(nf - 15.f, std::floor(pk / 10.f) * 10.f - 60.f);
            set_db_range(lo, lo + 90.f);
        }
        {
            std::lock_guard lk(mu_);
            frame_.seq++;
            frame_.db = db;
            frame_.max_hold = maxh;
            frame_.sample_rate = stream_.meta().sample_rate;
            frame_.center_freq = stream_.meta().center_freq;
            frame_.peak_db = db[pk];
            frame_.peak_offset_hz = dsp::SpectrumEstimator::bin_to_offset(pk, nfft_, frame_.sample_rate);
            frame_.noise_floor_db = sorted[nfft_ / 2];
            frame_.sample_index = h.sample_index;
            frame_.generation = h.generation;
            frame_.flags = flags;
            uint32_t* row = ring_.data() + (rows_written_.load() % rows_) * nfft_;
            for (std::size_t i = 0; i < nfft_; ++i) row[i] = colorize(db[i]);
            rows_written_++;
        }
    }
}

} // namespace spear::appfw
