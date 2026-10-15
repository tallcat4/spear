// JR TRAIN RADIO 受信機 — 純 C++(Qt / UHD / Stream Bus 非依存)。docs/apps/jr_train.md
//
//   radio.rx(in_rate)→ 回転(帯域中心を DC へ)→ FIR ÷decim(work_rate ≒ 240 kS/s)
//   → [全チャネル] jr_dsp::Demodulator(チャネルの NCO → 19.2 kS/s → RRC・タイミング・搬送波・MLSE → dibit)
//   → protocol::Synchronizer(384 bit フレーム、マーカー 2/3)
// work_rate 以降は原本(jr-conventional-line-radio-receiver)の 240 kS/s 経路と同じ。前段(回転 + FIR)は spear 側の追加。
//
// チャネル処理は worker スレッドに分ける(MLSE 64 状態 × 22 波)。process() は全チャネルの処理を待ってから返り、
// Observer はすべて呼び出し元のスレッドで呼ぶ(App 側は排他を考えなくてよい)。
#pragma once

#include "dsp/jr_dsp.hpp"
#include "protocol/frame_sync.hpp"
#include "spear/core/types.hpp"
#include "spear/dsp/fir.hpp"
#include "spear/dsp/stage.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace spear::jr_train {

struct ReceiverConfig {
    double in_rate = 1.92e6;                 // radio.rx
    double rotate_hz = 0;                    // 入力を −rotate_hz 回す: 帯域中心 − 実際の LO(録音再生なら 帯域中心 − 録音の中心)
    double work_rate_target = 240e3;         // decim = floor(in_rate / これ)(1 以上)。原本の既定 240 kS/s
    std::vector<double> channel_offsets_hz;  // 各チャネル − 帯域中心
    int threads = 0;                         // 0 = 自動(ハードウェアスレッド数 − 1、チャネル数まで)
};

struct Frame {
    int channel = 0;
    uint64_t epoch = 0;                      // チャネルの連続区間(discontinuity / 再同期で増える)
    uint64_t bit_offset = 0;                 // epoch 内の通算ビット位置
    bool sync_matched = false;
    uint32_t marker_errors = 0;
    std::array<uint8_t, protocol::kFrameBytes> received{};
    uint64_t input_sample_index = 0;         // radio.rx の sample index(概算: DSP の遅延は未較正。±数シンボル)
};

struct ChannelStatus {
    dsp::Snapshot snap;                      // 搬送波ロック・周波数誤差・シンボル数
    bool frame_locked = false;
    uint64_t frames = 0, synchronized_frames = 0;
    uint64_t epoch = 0;
    double evm_percent = 0;                  // 直近 256 シンボル(update_signal() の時点)
    uint64_t resets = 0;                     // DSP の作り直し(discontinuity・内部エラー)
};

struct Observer {
    std::function<void(std::span<const cf32> iq, uint64_t first_index)> band;   // 回転・間引き後(work_rate)。帯域俯瞰・録音用
    std::function<void(const Frame&)> frame;
};

class Receiver {
public:
    explicit Receiver(ReceiverConfig cfg);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    const ReceiverConfig& config() const { return cfg_; }
    double work_rate() const { return work_rate_; }
    std::size_t decim() const { return decim_; }
    int num_channels() const { return static_cast<int>(ch_.size()); }
    int threads() const { return std::max(1, static_cast<int>(workers_.size())); }
    void set_observer(Observer o) { obs_ = std::move(o); }

    // 入力ブロック(in_rate)。first_index = radio.rx の sample index(provenance)
    void process(std::span<const cf32> in, uint64_t first_index);
    // 連続性が切れた(欠落・generation 切替): 全チャネルの DSP と同期を作り直す
    void discontinuity();

    const ChannelStatus& status(int ch) const;
    // 指定チャネルのコンスタレーション(最大 capacity 点)と EVM を読み、status の evm_percent も更新する。process() と同じスレッドから呼ぶ
    double read_signal(int ch, std::vector<std::complex<float>>& points, std::size_t capacity = 256);
    // 全チャネルの EVM を更新する(表示の周期で呼ぶ)
    void update_signal();

    const spear::dsp::Provenance& provenance_to_work() const { return prov_; }

private:
    struct Channel;
    void work(std::size_t worker);
    void run_channel(Channel& c);

    ReceiverConfig cfg_;
    Observer obs_;
    std::size_t decim_ = 1;
    double work_rate_ = 0;
    std::unique_ptr<spear::dsp::FirDecimator<cf32>> front_;
    spear::dsp::Provenance prov_;          // work_rate の sample → radio.rx の sample
    std::vector<std::unique_ptr<Channel>> ch_;
    std::vector<cf32> rot_, work_buf_;
    double osc_re_ = 1, osc_im_ = 0, step_re_ = 1, step_im_ = 0;   // 回転の NCO
    std::size_t renorm_ = 0;
    uint64_t work_index_ = 0;              // work_rate の通し番号(discontinuity で 0 に戻さない)
    uint64_t in0_ = 0;
    bool have_in0_ = false;

    // worker
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::condition_variable cv_start_, cv_done_;
    uint64_t round_ = 0;
    std::size_t pending_ = 0;
    bool quit_ = false;
    std::span<const cf32> work_in_;
};

} // namespace spear::jr_train
