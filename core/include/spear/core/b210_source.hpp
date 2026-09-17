// S.P.E.A.R. core — B210LiveSource (要件 §7, §17.2)
//
// 実機 RX source。障害時の再接続責務を持つ:
//   * device 消失時、例外で App を落とさず再接続を試みる。成功したら generation を進め、
//     Discontinuity を打って継続する(記録を止めるより「切れたと明示して続ける」)。
//   * watchdog: recv() が timeout を返し続ける状態を検出し、能動的に再初期化する。
// UHD の rx_metadata_t を「区別したまま」Flags と Event に写す (§4.3, §17.1)。
// sample_index は time_spec の tick から導く: 欠落は index の飛びとして現れ、provenance が保たれる。
#pragma once

#include "radio.hpp"
#include "source.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

namespace spear {

struct B210RxStats {
    uint64_t blocks = 0, samples = 0;
    uint64_t overflow = 0, out_of_sequence = 0, timeout = 0, late_command = 0;
    uint64_t broken_chain = 0, alignment = 0, bad_packet = 0;
    uint64_t discontinuities = 0;    // time_spec 由来で検出した sample 欠落回数
    uint64_t missing_samples = 0;    // 同、欠落 sample 総数(推定)
    uint64_t reconnects = 0;
    uint64_t watchdog_resets = 0;
    uint64_t generation = 0;
    bool     streaming = false;
    int      startup_stage = 0;      // 現在到達している段 (1..6)。障害時は戻る
    std::string last_error;
};

struct B210SourceOptions {
    RadioOptions radio;
    double   recv_timeout_s = 0.5;
    int      watchdog_timeouts = 6;     // 連続 timeout 回数 → 再初期化 (§17.2 watchdog)
    double   reconnect_backoff_s = 1.0;
    double   probe_period_s = 0.5;      // device 不在時の probe 周期(副作用なし)
    double   fault_retry_s = 5.0;       // Fault 状態からの再試行間隔
    double   sensor_period_s = 1.0;     // 受信中のセンサ監視周期(lo_locked / temp / rssi / time drift)
    // 注: 受信中に FX3 状態レジスタを周期的に読むことはしない。EP0 ベンダ要求はバルク転送を
    // 約 13 ms 止める(USB 2.0 で実測、= 毎回 overflow)。受信中の一次ソースはデータ経路そのもので、
    // レジスタは遷移時と異常時(recv timeout)にのみ読んで原因を分類する。
    bool     realtime_priority = true;  // uhd::set_thread_priority_safe (§13.1 #3)
    std::size_t block_samples = kDefaultBlockSamples;
    std::size_t pool_blocks   = kDefaultPoolBlocks;
};

class B210LiveSource final : public Source {
public:
    B210LiveSource(EventBus* events, B210SourceOptions opt);
    ~B210LiveSource() override;

    std::string name() const override { return "b210"; }
    bool configure(const RfConfig& cfg, std::string* err = nullptr) override;
    void start() override;
    void stop() override;
    bool running() const override { return running_.load(); }
    Stream<sc16>& output() override { return *stream_; }
    uint64_t generation() const override { return generation_.load(); }
    RfConfig config() const override { std::lock_guard lk(cfg_mu_); return cfg_; }
    uint64_t state_version() const override { return state_version_.load() + radio_.state_version(); }

    // 任意スレッドから。宣言は即座に更新。RX thread が timed command で適用し、境界 sample を
    // Retune event で報告する。前の retune が確定するまで要求は保持される(最新値が勝ち、失われない)。
    void request_retune(double freq_hz);
    bool retune(double freq_hz) override { request_retune(freq_hz); return true; }

    B210RxStats stats() const;
    PoolStats pool_stats() const { return pool_.stats(); }
    const Radio& radio() const { return radio_; }
    DeviceStatus device_status() const override {
        auto d = radio_.status();
        d.generation = generation_.load();
        return d;
    }

private:
    void run();
    bool bring_up();          // 手順 1〜5
    void tear_down(bool device_lost);

    EventBus* events_;
    B210SourceOptions opt_;
    mutable std::mutex cfg_mu_;
    RfConfig cfg_;                          // cfg_mu_ で保護。RX thread はコピーを使う
    std::atomic<uint64_t> state_version_{0};
    Radio radio_;
    BlockPool pool_;
    std::unique_ptr<Stream<sc16>> stream_;
    uhd::rx_streamer::sptr rx_;

    std::thread th_;
    std::thread sensor_th_;                 // 受信中のセンサ監視(lo_locked / temp / rssi / drift)。安全性は実測済み
    std::atomic<bool> sensor_stop_{false};
    std::atomic<bool> running_{false}, stop_req_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<double> retune_req_{0.0}; // 0 = 無し。RX thread は適用できる時にだけ取り出す
    mutable std::mutex mu_;
    B210RxStats st_;
    uint64_t sequence_ = 0;
};

} // namespace spear
