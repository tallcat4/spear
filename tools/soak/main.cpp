// spear-soak — M-1: 前提の健全性確認 (要件 §14 M-1)
//
// 目的は sample rate の上限探しではなく、電源管理由来の断続性の確認。
//   10 Msps, 30 分連続受信、(a) AC + performance + USB autosuspend 無効 / (b) バッテリー + デフォルト
//   記録: overflow 回数 / 発生時刻分布 / CPU 周波数推移 / 温度
// 判定: (a) で 30 分ゼロ → M0 へ。(b) のみ発生 → 電源管理が原因確定。(a) でも発生 → UHD buffer / USB 構成。
#include "spear/core/b210_source.hpp"
#include "spear/core/event_log.hpp"
#include "spear/core/health.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <sys/mman.h>
#include <execinfo.h>
#include <exception>
#include <unistd.h>

using namespace spear;
using namespace std::chrono_literals;

namespace {
std::atomic<bool> g_stop{false};

// 診断用: terminate 時にどこから投げられたかを残す(destructor から投げられた例外は catch できない)
void on_terminate() {
    void* frames[64];
    const int n = backtrace(frames, 64);
    const char msg[] = "\n[soak] std::terminate — backtrace:\n";
    (void)!write(2, msg, sizeof msg - 1);
    backtrace_symbols_fd(frames, n, 2);
    if (auto ex = std::current_exception()) {
        try { std::rethrow_exception(ex); }
        catch (const std::exception& e) { std::fprintf(stderr, "[soak] exception: %s\n", e.what()); }
        catch (...) { std::fprintf(stderr, "[soak] non-std exception\n"); }
    }
    std::_Exit(134);
}
void on_sigint(int) { g_stop = true; }

const char* arg(int argc, char** argv, const char* key, const char* def) {
    for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return def;
}
bool flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], key)) return true;
    return false;
}
} // namespace

int main(int argc, char** argv) {
    if (flag(argc, argv, "--help")) {
        std::puts("spear-soak [--rate 10e6] [--freq 100e6] [--gain 30] [--minutes 30] [--out soak.csv]\n"
                  "           [--serial S] [--args 'num_recv_frames=128'] [--block 16384] [--mlock] [--no-rt]\n"
                  "           [--ant RX2] [--bw 0] [--event-log path.jsonl]");
        return 0;
    }
    RfConfig cfg;
    cfg.sample_rate = std::stod(arg(argc, argv, "--rate", "10e6"));
    cfg.center_freq = std::stod(arg(argc, argv, "--freq", "100e6"));
    cfg.gain        = std::stod(arg(argc, argv, "--gain", "30"));
    cfg.antenna     = arg(argc, argv, "--ant", "RX2");
    cfg.bandwidth   = std::stod(arg(argc, argv, "--bw", "0"));
    const double minutes = std::stod(arg(argc, argv, "--minutes", "30"));
    const std::string out = arg(argc, argv, "--out", "soak.csv");

    B210SourceOptions opt;
    opt.radio.expected_serial = arg(argc, argv, "--serial", "");
    opt.radio.device_args     = arg(argc, argv, "--args", "");
    opt.block_samples         = std::stoul(arg(argc, argv, "--block", "16384"));
    opt.realtime_priority     = !flag(argc, argv, "--no-rt");

    if (flag(argc, argv, "--mlock")) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) std::perror("mlockall");
        else std::puts("[soak] mlockall ok");
    }
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);
    std::set_terminate(on_terminate);

    EventBus events;
    // 起動シーケンス・異常を時刻付きで stdout へ(§12.1: 画面で診断が完結する前提の情報)
    const auto t_start = std::chrono::steady_clock::now();
    events.subscribe([&](const Event& e) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        if (e.kind == EventKind::ConsumerDrop) return;
        std::printf("[%9.3f] %-16s %-12s %s", t, std::string(to_string(e.kind)).c_str(), e.source.c_str(), e.detail.c_str());
        if (!e.range.empty() || e.range.begin) std::printf(" [gen %llu idx %llu..%llu]",
            (unsigned long long)e.range.generation, (unsigned long long)e.range.begin, (unsigned long long)e.range.end);
        if (e.value) std::printf(" (%lld)", (long long)e.value);
        std::printf("\n");
        std::fflush(stdout);
    });

    std::unique_ptr<EventLogFile> evlog;
    if (const char* p = arg(argc, argv, "--event-log", nullptr)) evlog = std::make_unique<EventLogFile>(events, p);
    B210LiveSource src(&events, opt);
    std::string err;
    if (!src.configure(cfg, &err)) { std::fprintf(stderr, "configure: %s\n", err.c_str()); return 1; }

    // Lossless consumer: 計測器としての基準。ここに drop があってはならない。
    auto sub = src.output().subscribe("soak_counter", DeliveryPolicy::Lossless, 256);
    std::atomic<uint64_t> got_samples{0}, got_blocks{0}, disc_seen{0};
    std::thread consumer([&] {
        ContinuityChecker c;
        while (!sub->eos()) {
            if (auto d = sub->pop(200ms)) {
                got_blocks++;
                got_samples += d->block.header().sample_count;
                if (d->block.header().sample_count && c.check(d->block.header())) disc_seen++;
                if (d->flags.has(Flag::Discontinuity)) disc_seen++;
            }
        }
    });

    std::ofstream csv(out);
    csv << "t_s,blocks,samples,overflow,out_of_sequence,timeout,late_command,discontinuities,missing_samples,"
           "reconnects,watchdog_resets,generation,consumer_dropped_blocks,pool_in_use,pool_heap_fallbacks,"
           "cpu_mhz_avg,cpu_mhz_min,governor,pkg_temp_c,throttle_count,on_ac,usb_autosuspend,load1\n";

    const auto h0 = read_health(".");
    std::printf("[soak] rate=%.3g freq=%.6g gain=%.1f minutes=%.1f governor=%s on_ac=%d usb_autosuspend=%d rt=%d\n",
                cfg.sample_rate, cfg.center_freq, cfg.gain, minutes, h0.governor.c_str(), h0.on_ac, h0.usb_autosuspend_on,
                opt.realtime_priority);
    src.start();

    const auto deadline = t_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(minutes * 60.0));
    auto next = t_start + 1s;
    B210RxStats last{};
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_until(next);
        next += 1s;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        const auto s = src.stats();
        const auto h = read_health(".");
        const auto cs = sub->stats();
        const auto ps = src.pool_stats();
        csv << t << ',' << s.blocks << ',' << s.samples << ',' << s.overflow << ',' << s.out_of_sequence << ',' << s.timeout << ','
            << s.late_command << ',' << s.discontinuities << ',' << s.missing_samples << ',' << s.reconnects << ',' << s.watchdog_resets << ','
            << s.generation << ',' << cs.dropped_blocks << ',' << ps.in_use << ',' << ps.heap_fallbacks << ','
            << h.cpu_mhz_avg << ',' << h.cpu_mhz_min << ',' << h.governor << ',' << h.pkg_temp_c << ','
            << (h.core_throttle_count + h.pkg_throttle_count) << ',' << h.on_ac << ',' << h.usb_autosuspend_on << ',' << h.load1 << '\n';
        csv.flush();
        if (static_cast<int>(t) % 10 == 0 || s.overflow != last.overflow || s.out_of_sequence != last.out_of_sequence) {
            const auto ds = src.device_status();
            std::printf("[%9.3f] %-12s rx %.1f Msps  O=%llu S=%llu T=%llu D=%llu(miss %llu) R=%llu  cdrop=%llu  cpu %.0f/%.0f MHz  %.0f C  thr=%llu"
                        "  | lo=%d ad9361 %.1f C rssi %.1f dB drift %+.2f±%.2f ppm\n",
                        t, std::string(to_string(ds.state)).c_str(),
                        static_cast<double>(s.samples - last.samples) / 1e6, (unsigned long long)s.overflow, (unsigned long long)s.out_of_sequence,
                        (unsigned long long)s.timeout, (unsigned long long)s.discontinuities, (unsigned long long)s.missing_samples,
                        (unsigned long long)s.reconnects, (unsigned long long)cs.dropped_blocks, h.cpu_mhz_avg, h.cpu_mhz_min,
                        h.pkg_temp_c, (unsigned long long)(h.core_throttle_count + h.pkg_throttle_count),
                        ds.sensors.lo_locked, ds.sensors.rx_temp_c, ds.sensors.rssi_db, ds.sensors.drift_ppm, ds.sensors.drift_uncertainty_ppm);
            std::fflush(stdout);
        }
        last = s;
    }
    src.stop();
    consumer.join();

    const auto s = src.stats();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::printf("\n==== soak summary (%.1f s) ====\n", t);
    std::printf("samples        %llu (%.3f Msps avg)\n", (unsigned long long)s.samples, static_cast<double>(s.samples) / t / 1e6);
    std::printf("overflow       %llu   <- host read too slowly (電源管理 / CPU)\n", (unsigned long long)s.overflow);
    std::printf("out_of_seq     %llu   <- transport packet loss (USB)\n", (unsigned long long)s.out_of_sequence);
    std::printf("timeout        %llu   watchdog_resets %llu\n", (unsigned long long)s.timeout, (unsigned long long)s.watchdog_resets);
    std::printf("discontinuity  %llu   missing_samples %llu\n", (unsigned long long)s.discontinuities, (unsigned long long)s.missing_samples);
    std::printf("reconnects     %llu   generation %llu\n", (unsigned long long)s.reconnects, (unsigned long long)s.generation);
    std::printf("consumer drops %llu   consumer disc seen %llu\n", (unsigned long long)sub->stats().dropped_blocks, (unsigned long long)disc_seen.load());
    std::printf("events: overflow=%llu oos=%llu timeout=%llu disconnected=%llu\n",
                (unsigned long long)events.count(EventKind::Overflow), (unsigned long long)events.count(EventKind::OutOfSequence),
                (unsigned long long)events.count(EventKind::Timeout), (unsigned long long)events.count(EventKind::Disconnected));
    {
        const auto ds = src.device_status();
        std::printf("device state   %s (%s)\n", std::string(to_string(ds.state)).c_str(), ds.evidence.c_str());
    }
    std::printf("verdict: %s\n", (s.overflow == 0 && s.out_of_sequence == 0 && s.discontinuities == 0 && s.reconnects == 0)
                                     ? "CLEAN (acceptance 10 条件を満たす)" : "NOT CLEAN — see csv for time distribution");
    return 0;
}
