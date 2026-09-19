// S.P.E.A.R. core — RecordingSource / SigmfRecorder (要件 §7, §8.4, M1)
//
// RecordingSource: SigMF 録音の再生。B210LiveSource と同一 API。
// SigmfRecorder:   Sink。IQ Recorder App が用いる。Lossless consumer の出力を SigMF + sidecar へ書く。
#pragma once

#include "sigmf.hpp"
#include "source.hpp"

#include <fstream>
#include <mutex>

namespace spear {

class RecordingSource final : public ThreadedSource {
public:
    // base_path は拡張子なし。open 失敗は例外ではなく running()==false + Error event。
    RecordingSource(EventBus* events, std::string base_path, bool loop = false,
                    std::size_t block_samples = kDefaultBlockSamples);
    bool open(std::string* err = nullptr);          // meta を読み cfg_ を録音条件で埋める
    const sigmf::Meta& meta() const { return meta_; }
    // 録音の rate / center は固定(データがそう)。config() は常に録音条件を返す(state-ownership: 宣言は実態を映す)。
    // App の宣言が違えば Warning を出して録音条件のまま起動する(offline 解析用途)。retune は無視して false。
    bool configure(const RfConfig& cfg, std::string* err = nullptr) override;
    bool retune(double center_freq_hz) override;
protected:
    uint32_t fill(BlockBuilder& b) override;
    void on_start() override;
    void on_stop() override;
private:
    bool reopen_data();
    std::string base_;
    bool loop_;
    bool opened_ = false;
    sigmf::Meta meta_;
    std::ifstream data_;
    uint64_t file_pos_samples_ = 0;
    std::size_t capture_idx_ = 0;
    std::vector<std::byte> tmp_;
};

class SigmfRecorder final : public Sink {
public:
    // lo_correction_ppm: Source::lo_correction_ppm()(録音時に LO 側で打ち消した個体誤差。SigMF global に provenance として残す)
    SigmfRecorder(std::string base_path, const StreamMeta& sm, const RfConfig& cfg, EventBus* events, double lo_correction_ppm = 0);
    ~SigmfRecorder() override;
    std::string name() const override { return "sigmf_recorder"; }
    void write(const Delivery& d) override;
    void close() override;
    bool ok() const { return ok_; }
    uint64_t samples_written() const { return written_; }
    const sigmf::Meta& meta() const { return meta_; }
private:
    void on_event(const Event& e);
    std::string base_;
    EventBus* events_;
    int listener_ = 0;
    std::ofstream data_;
    sigmf::Meta meta_;
    ContinuityChecker cont_;
    std::mutex mu_;
    bool ok_ = false, closed_ = false, first_ = true;
    uint64_t written_ = 0;
    uint64_t last_gen_ = 0;
    double initial_freq_ = 0.0;
};

} // namespace spear
