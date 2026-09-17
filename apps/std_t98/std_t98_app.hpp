// App: STD-T98 MONITOR (§14 M3) — docs/apps/std_t98.md
//
// 純 C++ の Receiver(30ch)を GUI に載せる。検証対象:
//   * §0 観測点: Receiver の中間 stream(帯域 IQ、選択 ch の IQ / アイパターン)を DSP に触れず GUI に出す
//   * §4.5 provenance: フレーム event の sample 範囲を radio.rx index で
//   * 個体設定(freq_err_hz)を --set / site.conf で渡す経路
//   * TraceSource / EyeDiagram(新規の再利用部品)
// RF: 4 Msps、LO は帯域中心から loOffset だけ離す(ゼロ IF の DC スパイクをチャネル 16 に重ねない)。
#pragma once

#include "ambe.hpp"
#include "receiver.hpp"
#include "spear/appfw/app.hpp"
#include "spear/appfw/trace_source.hpp"
#include "spear/appfw/view_source.hpp"
#include "spear/core/audio_sink.hpp"
#include "spear/core/block.hpp"
#include "spear/core/stream.hpp"

#include <QStringList>
#include <QVariantList>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace spear::apps {

class StdT98App final : public appfw::GuiApp {
    Q_OBJECT
    // ---- この App が所有する State ----
    // 帯域中心 / LO オフセット / 個体誤差は規格・App・site.conf が決める(UI からは操作しない: 誤設定で受信不能になる)
    Q_PROPERTY(double bandCenterHz READ bandCenterHz NOTIFY configChanged)   // ch16 の周波数(規格: 351.29375 MHz)
    Q_PROPERTY(double loOffsetHz READ loOffsetHz NOTIFY configChanged)
    Q_PROPERTY(double freqErrHz READ freqErrHz NOTIFY configChanged)         // 個体の LO 誤差(site.conf: std_t98.freq_err_hz)
    Q_PROPERTY(double squelchDb READ squelchDb WRITE setSquelchDb NOTIFY configChanged)
    Q_PROPERTY(int selectedChannel READ selectedChannel WRITE setSelectedChannel NOTIFY configChanged)   // 0..29
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY configChanged)
    Q_PROPERTY(bool mute READ mute WRITE setMute NOTIFY configChanged)
    Q_PROPERTY(bool allChannelAudio READ allChannelAudio WRITE setAllChannelAudio NOTIFY configChanged)   // 全 30ch を同時再生(ミックス)
    Q_PROPERTY(int numChannels READ numChannels CONSTANT)
    Q_PROPERTY(double channelSpacingHz READ channelSpacingHz CONSTANT)
    // ---- 観測値(DSP thread が更新、timer で通知)----
    Q_PROPERTY(QVariantList channels READ channels NOTIFY metersChanged)   // 30 × {power, open, frames, sacchOk, pichOk, csm, syncs, bestSse}
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY metersChanged)    // 選択 ch の詳細
    Q_PROPERTY(QStringList frameLog READ frameLog NOTIFY metersChanged)    // 直近フレーム(新しい順)
    Q_PROPERTY(double totalFrames READ totalFrames NOTIFY metersChanged)
    Q_PROPERTY(double losslessDrops READ losslessDrops NOTIFY metersChanged)
    Q_PROPERTY(double dspLoad READ dspLoad NOTIFY metersChanged)           // DSP thread の実時間比
    Q_PROPERTY(QString audioError READ audioError NOTIFY metersChanged)
    Q_PROPERTY(double audioUnderruns READ audioUnderruns NOTIFY metersChanged)
    Q_PROPERTY(double audioLateFrames READ audioLateFrames NOTIFY metersChanged)   // ミキサに遅れて届いたフレーム数
    // ---- 観測点 ----
    Q_PROPERTY(spear::appfw::ViewSource* view READ view CONSTANT)          // radio.rx
    Q_PROPERTY(spear::appfw::ViewSource* bandView READ bandView CONSTANT)  // std_t98.band(400 kHz、30ch 俯瞰)
    Q_PROPERTY(spear::appfw::ViewSource* channelView READ channelView CONSTANT)   // std_t98.channel(選択 ch 62.5 kHz)
    Q_PROPERTY(spear::appfw::TraceSource* eye READ eye CONSTANT)           // 選択 ch のアイパターン
public:
    explicit StdT98App(appfw::AppInfo info, QObject* parent = nullptr);
    ~StdT98App() override;
    void configure(const QVariantMap& settings) override;
    RfConfig declare_rf_config() const override;

    double bandCenterHz() const { return band_center_hz_; }
    void setBandCenterHz(double hz);
    double loOffsetHz() const { return lo_offset_hz_; }
    void setLoOffsetHz(double hz);
    double freqErrHz() const { return freq_err_hz_; }
    void setFreqErrHz(double hz) { freq_err_hz_ = hz; rebuild_ = true; Q_EMIT configChanged(); }
    double squelchDb() const { return squelch_db_; }
    void setSquelchDb(double v) { squelch_db_ = v; Q_EMIT configChanged(); }
    int selectedChannel() const { return selected_; }
    void setSelectedChannel(int ch);
    double volume() const { return volume_; }
    void setVolume(double v);
    bool mute() const { return mute_; }
    void setMute(bool m);
    bool allChannelAudio() const { return all_audio_; }
    void setAllChannelAudio(bool on) { all_audio_ = on; Q_EMIT configChanged(); }
    int numChannels() const { return cfg_.num_channels; }
    double channelSpacingHz() const { return cfg_.spacing_hz; }
    Q_INVOKABLE double channelFreqHz(int ch) const;   // 規格上の周波数(帯域中心 + オフセット)
    Q_INVOKABLE double channelOffsetHz(int ch) const { return (ch - cfg_.num_channels / 2) * cfg_.spacing_hz; }

    QVariantList channels() const;
    QVariantMap selected() const;
    QStringList frameLog() const { std::lock_guard<std::mutex> lk(mu_); return frame_log_; }
    double totalFrames() const { return static_cast<double>(total_frames_.load()); }
    double losslessDrops() const { return sub_ ? static_cast<double>(sub_->stats().dropped_blocks) : 0; }
    double dspLoad() const { return dsp_load_; }
    QString audioError() const { return audio_ ? QString::fromStdString(audio_->stats().error) : QString(); }
    double audioUnderruns() const { return audio_ ? static_cast<double>(audio_->stats().underruns) : 0; }
    double audioLateFrames() const { return static_cast<double>(audio_late_.load()); }
    appfw::ViewSource* view() { return &view_; }
    appfw::ViewSource* bandView() { return &band_view_; }
    appfw::ViewSource* channelView() { return &channel_view_; }
    appfw::TraceSource* eye() { return &eye_; }

Q_SIGNALS:
    void configChanged();
    void metersChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override;

private:
    void run();
    void retune();
    std::unique_ptr<std_t98::Receiver> build_receiver(Core& core);
    void publish(Stream<cf32>& s, BlockPool& pool, std::span<const cf32> x, uint64_t& seq, uint64_t& index, const BlockHeader& src);

    std_t98::ReceiverConfig cfg_;
    double band_center_hz_ = 351.29375e6;   // ARIB STD-T98: ch1 = 351.20000 MHz、6.25 kHz 間隔、ch16 が帯域中心
    double lo_offset_hz_ = 250e3;
    double freq_err_hz_ = 0;
    std::atomic<double> squelch_db_{-40.0};   // std-t98-tools の既定。録音再生では -50 が要る場合あり(site.conf: std_t98.squelch_db)
    std::atomic<int> selected_{0};
    double volume_ = 0.5;
    bool mute_ = false;
    std::atomic<bool> all_audio_{true};
    std::string audio_device_ = "default";
    std::atomic<bool> rebuild_{true};
    double built_center_ = 0;   // Receiver を組んだときの実 LO(変われば組み直す)

    // 観測値(DSP thread → GUI)
    mutable std::mutex mu_;
    struct ChanStat { double power_db = -999; bool open = false; uint64_t frames = 0, sacch_ok = 0, pich_ok = 0, syncs = 0; double best_sse = 1e9, sps = 0; std::string csm; double freq_err_est = 0; };
    std::vector<ChanStat> stats_;
    QVariantMap last_frame_;
    QStringList frame_log_;
    std::atomic<uint64_t> total_frames_{0};
    std::atomic<double> dsp_load_{0};

    appfw::ViewSource view_, band_view_, channel_view_;
    appfw::TraceSource eye_;
    std::unique_ptr<appfw::ViewProcessor> vp_, band_vp_, channel_vp_;
    std::unique_ptr<AudioSink> audio_;
    // 音声ミキサ(DSP thread 専用): 各チャネルのフレーム音声を provenance(radio.rx index → 8 kHz index)の位置に加算し、
    // 入力の進みから一定の遅延(kMixLatency)を置いた位置まで AudioSink に流す。全チャネル同時再生も 1 チャネルも同じ経路。
    struct Mixer {
        std::vector<float> ring;         // 絶対 8 kHz index の mod で参照
        uint64_t play_pos = 0;           // 次に AudioSink へ出す絶対 index
        bool started = false;
        uint64_t last_reached = 0;
    } mix_;
    std::vector<std::unique_ptr<std_t98::AmbeDecoder>> decoders_;   // チャネルごと(DSP thread 専用)
    std::atomic<uint64_t> audio_late_{0};
    void mix_frame(const std_t98::Frame& f);
    void mix_flush(uint64_t reached_input_index);
    std::unique_ptr<BlockPool> band_pool_, channel_pool_;
    std::unique_ptr<Stream<cf32>> band_, channel_;
    std::shared_ptr<Subscription> sub_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    int timer_ = 0;
    uint64_t band_seq_ = 0, band_index_ = 0, chan_seq_ = 0, chan_index_ = 0;
};

} // namespace spear::apps
