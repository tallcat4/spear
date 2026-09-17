// S.P.E.A.R. core — Source / Sink 抽象 (要件 §7)
//
// App から見て入出力先が区別されない。B210LiveSource / RecordingSource / SyntheticSource は
// 同じ Stream<sc16> "radio.rx" を出力し、同じ App コードへ差し替えて流し込める(§12.3 の土台)。
// 出力型は B210 native の sc16 に統一する(10 Msps = 40 MB/s, §1.1)。
#pragma once

#include "stream.hpp"
#include "types.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace spear {

inline constexpr const char* kRadioRxStreamId = "radio.rx";
inline constexpr std::size_t kDefaultBlockSamples = 16384;  // 10 Msps で ~1.6 ms / block
inline constexpr std::size_t kDefaultPoolBlocks   = 256;    // 16384 * 4 B * 256 = 16 MB (§13.2 予算内)

// ---- State の単一所有 (§4.1) ----
// 「宣言された RF 条件」は Source が 1 か所で所有する。GUI や App はコピーを表示に使わず、
// config() を読む。変更は必ず Source の API(configure / retune)を通り、state_version が進む。
// 観測側は state_version の変化で「何かが変わった」ことを安価に検知できる。
class Source {
public:
    virtual ~Source() = default;
    virtual std::string name() const = 0;

    // App の宣言 (§8.1) を適用する。適用不能なら false(App 起動失敗)。start 前に呼ぶ。
    virtual bool configure(const RfConfig& cfg, std::string* err = nullptr) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;

    virtual Stream<sc16>& output() = 0;
    virtual uint64_t generation() const = 0;

    // 現在の宣言(コピー、thread-safe)。retune 要求は即座にここへ反映される。
    virtual RfConfig config() const = 0;
    // 宣言・装置状態が変わるたびに進む。
    virtual uint64_t state_version() const = 0;

    // 装置状態 (§12.1)。実機以外は Ready / Streaming のみを返す。
    virtual DeviceStatus device_status() const = 0;

    // 運転中の retune 要求(timed command, §17.1)。宣言は即座に更新され、実際に適用された時点で
    // Retune event(value = 実周波数, range.begin = 境界 sample index)が出る。要求は失われない(最新値が勝つ)。
    virtual bool retune(double center_freq_hz) = 0;

    // ---- 立ち上げ(起動時、App を始める前)----
    // 実機なら自己診断 → 装置の出現待ち → open(FPGA ロード)→ tune 確認 → close まで裏で進め、装置を「すぐ使える」状態にする。
    // 進行中は warming_up() が true、経過は startup_report()(行の列)と device_status()(状態・進捗)で見える。
    // start() は warm_up の完了を待ってから走る。実機以外では何もしない。
    virtual void warm_up() {}
    virtual bool warming_up() const { return false; }
    virtual void cancel_warm_up() {}                                  // 装置の出現待ちを打ち切る(open 中は打ち切れない)
    virtual std::vector<std::string> startup_report() const { return {}; }
};

class Sink {
public:
    virtual ~Sink() = default;
    virtual std::string name() const = 0;
    virtual void write(const Delivery& d) = 0;  // consumer thread から
    virtual void close() {}
};

class NullSink final : public Sink {
public:
    std::string name() const override { return "null"; }
    void write(const Delivery& d) override { blocks_++; samples_ += d.block.header().sample_count; }
    uint64_t blocks() const { return blocks_; }
    uint64_t samples() const { return samples_; }
private:
    uint64_t blocks_ = 0, samples_ = 0;
};

// 共通: 一定の block を生成 thread から publish する source の土台。
// Synthetic / Recording が共有。B210LiveSource は UHD 固有の loop を持つので別。
class ThreadedSource : public Source {
public:
    ThreadedSource(std::string name, EventBus* events, std::size_t block_samples = kDefaultBlockSamples,
                   std::size_t pool_blocks = kDefaultPoolBlocks);
    ~ThreadedSource() override;

    std::string name() const override { return name_; }
    bool configure(const RfConfig& cfg, std::string* err = nullptr) override;
    void start() override;
    void stop() override;
    bool running() const override { return running_.load(); }
    Stream<sc16>& output() override { return *stream_; }
    uint64_t generation() const override { return generation_.load(); }
    RfConfig config() const override { std::lock_guard lk(cfg_mu_); return cfg_; }
    uint64_t state_version() const override { return state_version_.load(); }
    // ハードウェア無し: 宣言を即時更新し、同じ Retune event 経路を通す(App/GUI は実機と区別しない)
    bool retune(double center_freq_hz) override;

    // true なら実時間でペーシング。false なら可能な限り速く(テスト・offline 再解析)。
    void set_realtime(bool rt) { realtime_ = rt; }
    const PoolStats pool_stats() const { return pool_.stats(); }
    DeviceStatus device_status() const override {
        DeviceStatus d;
        d.state = running_ ? DeviceState::Streaming : DeviceState::Ready;
        d.evidence = name_ + " (no hardware)";
        d.stage = running_ ? 6 : 4;
        d.generation = generation_.load();
        d.since_ns = state_since_ns_.load();
        return d;
    }
    uint64_t current_sample_index() const { return sample_index_pub_.load(); }

protected:
    // 派生: 1 block 分を builder に書き、sample 数を返す。0 なら EOS。
    // flags を立てたければ builder.header().flags に。
    virtual uint32_t fill(BlockBuilder& b) = 0;
    virtual void on_start() {}
    virtual void on_stop() {}
    void next_generation();  // 時間軸の切断(loop 再生等)
    void bump_state() { state_version_++; }

    std::string name_;
    EventBus*   events_;
    mutable std::mutex cfg_mu_;
    RfConfig    cfg_;                      // cfg_mu_ で保護。RX thread は config() のコピーを使う
    std::atomic<uint64_t> state_version_{0};
    std::size_t block_samples_;
    BlockPool   pool_;
    std::unique_ptr<Stream<sc16>> stream_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> state_since_ns_{0};
    bool realtime_ = true;

private:
    void run();
    std::thread th_;
    std::atomic<bool> stop_req_{false};
    uint64_t sequence_ = 0;
    uint64_t sample_index_ = 0;
    std::atomic<uint64_t> sample_index_pub_{0};   // retune event の範囲に使う(RX thread が更新)
};

} // namespace spear
