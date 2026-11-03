// spear-replay — 録音 IQ の送信を GUI なしで確かめる(要件 §6, §7.2 Replay Transmitter, §4.3 の TX 不変条件)
//
//   spear-replay <sigmf base> [--source b210|synthetic] [--tx-gain dB] [--port TRXA|TRXB] [--loops N]
//                [--serial S] [--freq-err-ppm P] [--idle-s S] [--seconds S] --arm
//
// 録音の center / rate で装置を送信に構成し、録音を N 回つなげて 1 つの burst として送る(既定 1 回)。
// 送信ゲインの既定は 0 dB(最小)。必ず減衰器 / ダミーロードを付けた状態で使う。
// --arm: Core の TX MASTER を ARMED にする。付けなければ Core が送信を拒否する(GUI のメニューの TX MASTER と同じ仕組み)。
// --seconds: 先頭から S 秒分だけ送る(長い録音の一部で確かめる)。
// --idle-s: burst の前に streamer を開いたまま待つ秒数(burst の外で何も出ていないことを受信機で確かめる用)。
#include "spear/core/core.hpp"
#include "spear/core/recording.hpp"
#include "spear/core/synthetic_source.hpp"
#if SPEAR_WITH_UHD
#include "spear/core/b210_source.hpp"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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

class ReplayApp final : public App {
public:
    ReplayApp(std::string base, RfConfig cfg, int loops, double idle_s, uint64_t limit)
        : base_(std::move(base)), cfg_(cfg), loops_(loops), idle_s_(idle_s), limit_(limit) {}
    std::string name() const override { return "replay_headless"; }
    RfConfig declare_rf_config() const override { return cfg_; }
    void start(Core& core) override {
        tx_ = &core.tx();
        th_ = std::thread([this] { run(); });
    }
    void stop() override { if (th_.joinable()) th_.join(); }
    bool done() const { return done_; }
private:
    void run() {
        for (int i = 0; i < static_cast<int>(idle_s_ * 10) && !g_stop; ++i) std::this_thread::sleep_for(100ms);
        SigmfReader r;
        std::string err;
        if (!r.open(base_, &err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); done_ = true; return; }
        auto& pool = pool_;
        bool first = true;
        uint64_t total = 0;
        for (int loop = 0; loop < loops_ && !g_stop; ++loop) {
            if (loop > 0 && !r.rewind()) break;
            while (!g_stop) {
                auto b = pool.acquire<sc16>(kDefaultBlockSamples);
                const std::size_t want = limit_ ? static_cast<std::size_t>(std::min<uint64_t>(kDefaultBlockSamples, limit_ - total)) : kDefaultBlockSamples;
                const std::size_t n = want ? r.read(b.data<sc16>().data(), want) : 0;
                if (n == 0) break;
                total += n;
                if (first) { b.header().flags.set(Flag::StartOfBurst); first = false; }
                if (!tx_->write(b.commit(static_cast<uint32_t>(n)))) { done_ = true; return; }
            }
        }
        auto eob = pool.acquire<sc16>(1);
        eob.header().flags.set(Flag::EndOfBurst);
        tx_->write(eob.commit(0));
        done_ = true;
    }
    std::string base_;
    RfConfig cfg_;
    int loops_;
    double idle_s_;
    uint64_t limit_;   // 0 = 全部
    TxStream* tx_ = nullptr;
    BlockPool pool_{kDefaultBlockSamples * sizeof(sc16), kDefaultTxQueueBlocks + 8};   // 送信キューの block が指す。Source が止まるまで生かす
    std::thread th_;
    std::atomic<bool> done_{false};
};
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argv[1][0] == '-') {
        std::fprintf(stderr, "usage: %s <sigmf base> [--source b210|synthetic] [--tx-gain dB] [--port TRXA|TRXB] [--loops N] [--serial S] [--freq-err-ppm P] [--idle-s S] [--seconds S] --arm\n", argv[0]);
        return 2;
    }
    std::signal(SIGINT, on_sigint);
    const std::string base = argv[1];
    SigmfReader probe;
    std::string err;
    if (!probe.open(base, &err)) { std::fprintf(stderr, "%s: %s\n", base.c_str(), err.c_str()); return 1; }
    RfConfig cfg;
    cfg.direction = Direction::TX;
    cfg.sample_rate = probe.meta().sample_rate;
    cfg.center_freq = probe.meta().captures.empty() ? 0.0 : probe.meta().captures.front().frequency;
    cfg.tx_gain = std::atof(arg(argc, argv, "--tx-gain", "0"));
    if (!parse_rf_port(arg(argc, argv, "--port", "TRXA"), &cfg.tx_port)) { std::fprintf(stderr, "unknown port\n"); return 2; }
    const int loops = std::max(1, std::atoi(arg(argc, argv, "--loops", "1")));
    const double idle_s = std::atof(arg(argc, argv, "--idle-s", "0"));
    std::printf("file: %s  %.6f MHz  %.0f sps  %llu samples (%.2f s) x %d, tx_gain %.2f dB, port %s\n", base.c_str(), cfg.center_freq / 1e6,
                cfg.sample_rate, static_cast<unsigned long long>(probe.total_samples()), probe.total_samples() / cfg.sample_rate, loops,
                cfg.tx_gain, std::string(rf_port_name(cfg.tx_port)).c_str());
    probe.close();

    Core core(".");
    core.events().subscribe([](const Event& e) {
        switch (e.kind) {
        case EventKind::RxGain: case EventKind::UhdLog: return;
        default: break;
        }
        std::printf("[%s] %s: %s  (g%llu %llu..%llu, v=%lld)\n", std::string(to_string(e.kind)).c_str(), e.source.c_str(), e.detail.c_str(),
                    static_cast<unsigned long long>(e.range.generation), static_cast<unsigned long long>(e.range.begin),
                    static_cast<unsigned long long>(e.range.end), static_cast<long long>(e.value));
    });
    const std::string source = arg(argc, argv, "--source", "b210");
    std::unique_ptr<Source> src;
    if (source == "synthetic") {
        src = std::make_unique<SyntheticSource>(&core.events(), SyntheticSignal{});
    } else {
#if SPEAR_WITH_UHD
        B210SourceOptions opt;
        opt.radio.expected_serial = arg(argc, argv, "--serial", "");
        opt.radio.freq_err_ppm = std::atof(arg(argc, argv, "--freq-err-ppm", "0"));
        src = std::make_unique<B210LiveSource>(&core.events(), opt);
#else
        std::fprintf(stderr, "built without UHD\n");
        return 1;
#endif
    }
    core.set_source(std::move(src));
    core.set_tx_armed(flag(argc, argv, "--arm"));
    if (!core.tx_armed()) std::printf("TX MASTER is SAFE (no --arm): the core will refuse to transmit\n");
    const uint64_t limit = static_cast<uint64_t>(std::atof(arg(argc, argv, "--seconds", "0")) * cfg.sample_rate);
    auto app = std::make_shared<ReplayApp>(base, cfg, loops, idle_s, limit);
    core.set_ready_timeout(30);
    if (!core.run_app(app, &err)) { std::fprintf(stderr, "run_app: %s\n", err.c_str()); return 1; }
    // burst を出し切るまで待つ(App が EOB を入れ、装置が burst を閉じる)
    auto& tx = core.tx();
    while (!g_stop) {
        const auto st = tx.stats();
        if (app->done() && !st.in_burst && st.queue_depth == 0) break;
        std::this_thread::sleep_for(100ms);
    }
    std::this_thread::sleep_for(300ms);   // burst ack / 最後の async msg
    const auto st = tx.stats();
    core.stop_app();
    core.events().flush();
    std::printf("sent %llu samples in %llu blocks, bursts %llu, underflows %llu, seq_errors %llu, time_errors %llu, max queue %zu/%zu\n",
                static_cast<unsigned long long>(st.sent_samples), static_cast<unsigned long long>(st.sent_blocks),
                static_cast<unsigned long long>(st.bursts), static_cast<unsigned long long>(st.underflows),
                static_cast<unsigned long long>(st.seq_errors), static_cast<unsigned long long>(st.time_errors), st.max_depth, st.queue_capacity);
    return st.underflows == 0 && st.seq_errors == 0 ? 0 : 3;
}
