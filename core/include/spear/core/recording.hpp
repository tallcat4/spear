// S.P.E.A.R. core — RecordingSource / SigmfRecorder (要件 §6, §7.4, M1)
//
// RecordingSource: SigMF 録音の再生。B210LiveSource と同一 API。
// SigmfRecorder:   Sink。IQ Recorder App が用いる。Lossless consumer の出力を SigMF + sidecar へ書く。
// SigmfReader:     録音の sample を sc16 で順に読む(RecordingSource と送信 App が共有)。
#pragma once

#include "sigmf.hpp"
#include "source.hpp"

#include <fstream>
#include <atomic>
#include <mutex>

namespace spear {

// 録音の data file を先頭から sc16 で読む。cf32 の録音は full scale = 1.0 として sc16 に量子化する。
class SigmfReader {
public:
    // base_path は拡張子なし。meta(.sigmf-meta + sidecar)と data を開く
    bool open(const std::string& base_path, std::string* err = nullptr);
    void close() { data_.close(); }
    bool is_open() const { return data_.is_open(); }
    const sigmf::Meta& meta() const { return meta_; }
    uint64_t total_samples() const { return total_; }   // data file の大きさから
    uint64_t position() const { return pos_; }          // 次に読む sample(data file 内)
    // 最大 n sample を読む。末尾なら 0
    std::size_t read(sc16* out, std::size_t n);
    bool rewind();
private:
    std::string base_;
    sigmf::Meta meta_;
    std::ifstream data_;
    uint64_t total_ = 0, pos_ = 0;
    std::vector<std::byte> tmp_;
};

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
    SigmfReader reader_;
    std::size_t capture_idx_ = 0;
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
    std::atomic<bool> ok_{false};                 // write() の失敗で false(他 thread から ok() で読む)
    bool closed_ = false, first_ = true;
    std::atomic<uint64_t> written_{0};
    uint64_t last_gen_ = 0;
    double initial_freq_ = 0.0;
};

} // namespace spear
