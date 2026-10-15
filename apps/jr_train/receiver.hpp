// JR TRAIN RADIO 受信機 — 純 C++(Qt / UHD / Stream Bus 非依存)。docs/apps/jr_train.md
//
//   radio.rx(in_rate)→ 回転(帯域中心を DC へ)→ FIR ÷decim(work_rate ≒ 240 kS/s)
//   → [全チャネル] Pipeline(原本 pipeline.rs: 受信 DSP → フレーム同期 → 音声フレームの誤り訂正 → 音声合成 / データ電文)
// work_rate 以降は原本(jr-conventional-line-radio-receiver)の 240 kS/s 経路と同じ。前段(回転 + FIR)は spear 側の追加。
//
// チャネル処理は worker スレッドに分ける(MLSE 64 状態と音声合成 × 22 波)。process() は全チャネルの処理を待ってから返り、
// Observer はすべて呼び出し元のスレッドで、チャネルの順に(各チャネル内ではフレーム → 音声の順に)呼ぶ。
#pragma once

#include "pipeline.hpp"
#include "spear/core/types.hpp"
#include "spear/dsp/fir.hpp"
#include "spear/dsp/stage.hpp"

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
    double band_center_hz = 0;               // 帯域中心(チャネルのオフセットの基準)
    std::vector<ChannelConfig> channels;
    int threads = 0;                         // 0 = 自動(ハードウェアスレッド数 − 1、チャネル数まで)
};

// フレームの時刻(provenance)
struct FrameTiming {
    uint64_t input_sample_index = 0;         // radio.rx の sample index(概算: DSP の遅延は未較正。±数シンボル)
    uint64_t time_us = 0;                    // 原本 session の時刻: 区間の開始 + bit_offset / 9600 s。通話の優先順位に使う
};

struct Observer {
    std::function<void(std::span<const cf32> iq, uint64_t first_index)> band;   // 回転・間引き後(work_rate)
    std::function<void(int ch, const FrameEvent& ev, const FrameTiming& t)> frame;
    std::function<void(int ch, const Audio& a)> audio;                       // 8 kHz PCM 320 sample(40 ms)
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
    // 連続性が切れた(欠落・generation 切替): 全チャネルの DSP・同期・音声・組立を作り直す(原本 Pipeline::discontinuity)
    void discontinuity();
    // 入力の終わり: 音声合成の先読みを吐き出す(録音の再生終了時など)
    void flush();

    // 状態(process() と同じスレッドから読む)。snapshot は EVM・コンスタレーション・電文の履歴を更新する
    const PipelineStatus& pipeline_status(int ch) const;
    const PipelineStatus& snapshot(int ch);
    std::vector<protocol::Message> take_messages(int ch);
    uint64_t resets(int ch) const;           // 内部エラーで作り直した回数
    uint64_t time_us() const;                // 処理済みの時刻(原本 session の progress_us)

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
    uint64_t segment_start_ = 0;           // 現在の連続区間が始まった work_rate の番号
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
