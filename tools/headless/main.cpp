// spear-headless — M0: Stream Bus の成立を GUI なしで確認する (要件 §11, §14 M0)
//
//   Source → Stream Bus → { spectrum (LatestOnly), waterfall (LatestOnly, わざと遅い), recorder (Lossless) }
//
//   * 同一 IQ stream を 3 consumer が同時利用する
//   * waterfall の起動・停止(途中で attach/detach)が他 consumer へ影響しない
//   * consumer ごとの drop 統計が取得できる
// source は b210 / synthetic / file:<base> を同一コードで差し替えられる (§7)。
#include "spear/core/core.hpp"
#include "spear/core/recording.hpp"
#include "spear/core/synthetic_source.hpp"
#include "spear/core/tap.hpp"
#include "spear/dsp/spectrum.hpp"
#if SPEAR_WITH_UHD
#include "spear/core/b210_source.hpp"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

using namespace spear;
using namespace std::chrono_literals;

namespace {
std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop = true; }
const char* arg(int argc, char** argv, const char* key, const char* def) {
    for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return def;
}
bool flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], key)) return true;
    return false;
}

// ---- M0 の 3 consumer を持つ App。producer(source)は App の存在を知らない。----
class M0App final : public App {
public:
    M0App(RfConfig cfg, std::string record_base, int waterfall_delay_ms)
        : cfg_(cfg), record_base_(std::move(record_base)), wf_delay_ms_(waterfall_delay_ms) {}

    std::string name() const override { return "m0_headless"; }
    RfConfig declare_rf_config() const override { return cfg_; }

    void start(Core& core) override {
        core_ = &core;
        stop_ = false;
        auto& rx = core.rx();
        // consumer 1: spectrum (LatestOnly)
        spec_sub_ = rx.subscribe("spectrum", DeliveryPolicy::LatestOnly, 2);
        spec_th_ = std::thread([this] { spectrum_loop(); });
        // consumer 3: recorder (Lossless)
        if (!record_base_.empty()) {
            rec_ = std::make_unique<SigmfRecorder>(record_base_, rx.meta(), cfg_, &core_->events());
            rec_sub_ = rx.subscribe("recorder", DeliveryPolicy::Lossless, 128);
            rec_th_ = std::thread([this] { recorder_loop(); });
        }
        // consumer 2: waterfall は途中から attach する(→ 影響を見る)
    }

    void attach_waterfall() {
        if (wf_sub_) return;
        wf_sub_ = core_->rx().subscribe("waterfall", DeliveryPolicy::LatestOnly, 1);
        wf_th_ = std::thread([this] { waterfall_loop(); });
        std::printf("[app] waterfall attached\n");
    }
    void detach_waterfall() {
        if (!wf_sub_) return;
        wf_quit_ = true;
        wf_th_.join();
        wf_sub_.reset();
        wf_quit_ = false;
        std::printf("[app] waterfall detached\n");
    }

    void stop() override {
        stop_ = true;
        detach_waterfall();
        if (spec_th_.joinable()) spec_th_.join();
        if (rec_th_.joinable()) rec_th_.join();
        if (rec_) { rec_->close(); std::printf("[app] recorded %llu samples → %s\n", (unsigned long long)rec_->samples_written(), record_base_.c_str()); }
        spec_sub_.reset(); rec_sub_.reset();
    }

    void print_stats() {
        for (const auto& s : core_->rx().stats())
            std::printf("    %-10s %-11s delivered=%-8llu dropped=%-6llu bursts=%-3llu depth=%zu/%zu max=%zu\n",
                        s.name.c_str(), std::string(to_string(s.policy)).c_str(), (unsigned long long)s.delivered_blocks,
                        (unsigned long long)s.dropped_blocks, (unsigned long long)s.overflow_bursts, s.queue_depth, s.queue_capacity, s.max_depth);
        std::printf("    peak: %+7.1f dBFS @ %+9.1f kHz\n", peak_db_.load(), peak_hz_.load() / 1e3);
    }

private:
    void spectrum_loop() {
        dsp::SpectrumEstimator est(1024);
        std::vector<float> db;
        while (!stop_) {
            auto d = spec_sub_->pop(100ms);
            if (!d || d->block.header().sample_count < 1024) continue;
            auto iq = d->block.as<sc16>();
            TAP("headless.spectrum_in", iq.data(), iq.size()); // 中間 stream の観測点 (§5.3)
            if (!est.compute_averaged(iq, db)) continue;
            std::size_t pk = 0;
            for (std::size_t i = 1; i < db.size(); ++i) if (db[i] > db[pk]) pk = i;
            peak_db_ = db[pk];
            peak_hz_ = static_cast<float>(dsp::SpectrumEstimator::bin_to_offset(pk, 1024, core_->rx().meta().sample_rate));
        }
    }
    void waterfall_loop() {
        // GUI が遅い状況を模す: 1 block ごとに sleep。LatestOnly なので自分の stream だけ drop する。
        while (!wf_quit_) {
            if (auto d = wf_sub_->pop(100ms)) std::this_thread::sleep_for(std::chrono::milliseconds(wf_delay_ms_));
        }
    }
    void recorder_loop() {
        while (!stop_) if (auto d = rec_sub_->pop(100ms)) rec_->write(*d);
    }

    RfConfig cfg_;
    std::string record_base_;
    int wf_delay_ms_;
    Core* core_ = nullptr;
    std::atomic<bool> stop_{false}, wf_quit_{false};
    std::shared_ptr<Subscription> spec_sub_, wf_sub_, rec_sub_;
    std::thread spec_th_, wf_th_, rec_th_;
    std::unique_ptr<SigmfRecorder> rec_;
    std::atomic<float> peak_db_{-999.f}, peak_hz_{0.f};
};

} // namespace

int main(int argc, char** argv) {
    if (flag(argc, argv, "--help")) {
        std::puts("spear-headless --source b210|synthetic|file:<base> [--rate 4e6] [--freq 100e6] [--gain 30]\n"
                  "               [--seconds 20] [--record <base>] [--wf-delay-ms 50] [--serial S] [--args UHD_ARGS]");
        return 0;
    }
    const std::string source = arg(argc, argv, "--source", "synthetic");
    RfConfig cfg;
    cfg.sample_rate = std::stod(arg(argc, argv, "--rate", "4e6"));
    cfg.center_freq = std::stod(arg(argc, argv, "--freq", "100e6"));
    cfg.gain        = std::stod(arg(argc, argv, "--gain", "30"));
    const double seconds = std::stod(arg(argc, argv, "--seconds", "20"));
    const std::string record = arg(argc, argv, "--record", "");
    const int wf_delay = std::stoi(arg(argc, argv, "--wf-delay-ms", "50"));

    std::signal(SIGINT, on_sigint);

    Core core(".");
    const auto t0 = std::chrono::steady_clock::now();
    core.events().subscribe([&](const Event& e) {
        if (e.kind == EventKind::ConsumerDrop) return;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[%8.3f] %-16s %-22s %s", t, std::string(to_string(e.kind)).c_str(), e.source.c_str(), e.detail.c_str());
        if (e.range.begin || e.range.end) std::printf(" [gen %llu %llu..%llu]", (unsigned long long)e.range.generation,
                                                       (unsigned long long)e.range.begin, (unsigned long long)e.range.end);
        if (e.value) std::printf(" (%lld)", (long long)e.value);
        std::printf("\n");
    });

    // ---- source 差し替え (§7): App コードは変わらない ----
    std::unique_ptr<Source> src;
    if (source == "synthetic") {
        SyntheticSignal sig;
        sig.tones = {{cfg.sample_rate * 0.1, 0.3}, {-cfg.sample_rate * 0.23, 0.1}};
        sig.noise_amplitude = 0.005;
        src = std::make_unique<SyntheticSource>(&core.events(), sig);
    } else if (source.rfind("file:", 0) == 0) {
        auto rs = std::make_unique<RecordingSource>(&core.events(), source.substr(5), true);
        std::string err;
        if (!rs->open(&err)) { std::fprintf(stderr, "open recording: %s\n", err.c_str()); return 1; }
        cfg = rs->config();
        src = std::move(rs);
    }
#if SPEAR_WITH_UHD
    else if (source == "b210") {
        B210SourceOptions opt;
        opt.radio.expected_serial = arg(argc, argv, "--serial", "");
        opt.radio.device_args = arg(argc, argv, "--args", "");
        src = std::make_unique<B210LiveSource>(&core.events(), opt);
    }
#endif
    else { std::fprintf(stderr, "unknown source %s\n", source.c_str()); return 1; }
    core.set_source(std::move(src));

    auto app = std::make_shared<M0App>(cfg, record, wf_delay);
    std::string err;
    if (!core.run_app(app, &err)) { std::fprintf(stderr, "run_app: %s\n", err.c_str()); return 1; }
    std::printf("[headless] source=%s rate=%.3g freq=%.6g seconds=%.0f\n", source.c_str(), cfg.sample_rate, cfg.center_freq, seconds);

    const auto deadline = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
    int tick = 0;
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1s);
        ++tick;
        // 1/3 経過で waterfall attach、2/3 で detach → recorder の drop が変わらないことを見る
        if (tick == static_cast<int>(seconds / 3)) app->attach_waterfall();
        if (tick == static_cast<int>(2 * seconds / 3)) app->detach_waterfall();
        std::printf("[%4d s] consumers:\n", tick);
        app->print_stats();
        std::fflush(stdout);
    }
    core.stop_app();
    std::printf("\n==== final ====\n");
    app->print_stats();
    bool ok = true;
    for (const auto& st : core.rx().stats()) if (st.policy == DeliveryPolicy::Lossless && st.dropped_blocks) ok = false;
    std::printf("lossless drops: %s\n", ok ? "ZERO" : "NON-ZERO");
    return ok ? 0 : 2;
}
