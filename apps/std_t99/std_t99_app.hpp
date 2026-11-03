// App: STD-T99 DIGICOM — ARIB STD-T99 Annex 2(デジタル小電力コミュニティ無線、デジコミ)の選択チャネル受信。docs/apps/std_t99.md
//
// 原本は std-t99-receiver-sharp(SDR# プラグイン)。純 C++ の Monitor(Front → Receiver → Session → CallMonitor → 音声の時間軸)を
// GUI に載せる。並列受信(全チャネル同時)は持たない: 選択した 1 チャネルだけを受け、チャネルを変えると LO を動かす。
//   * RF: 4 Msps、LO = 選択チャネル − loOffset(250 kHz。ゼロ IF の DC スパイクをチャネルから離す)。CH1–9 と CH10–18 は 4 MHz 離れている
//   * 音声: TOKUDER(tokuder48_causal_v1)は専用の復号スレッド(VoiceWorker)。待受(選択呼出)で再生を決める。保存は常に全通話
//   * §3.6 provenance: 送信の開始 / 終了の event に radio.rx の sample 範囲
//   * 観測点: std_t99.band(400 kHz)/ std_t99.channel(100 kHz)/ アイパターン / std_t99.audio(8 kHz)
//   * 保存: SAVE = 復号結果(.burst / .llr / .jsonl / .wav)、REC IQ = radio.rx の SigMF(golden 用)
#pragma once

#include <optional>

#include "channels.hpp"
#include "monitor.hpp"
#include "storage.hpp"
#include "voice/worker.hpp"
#include "spear/appfw/app.hpp"
#include "spear/appfw/speech_level.hpp"
#include "spear/appfw/trace_source.hpp"
#include "spear/appfw/view_source.hpp"
#include "spear/core/audio_sink.hpp"
#include "spear/core/block.hpp"
#include "spear/core/recording.hpp"
#include "spear/core/stream.hpp"

#include <QVariantList>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace spear::apps {

class StdT99App final : public appfw::GuiApp {
    Q_OBJECT
    // ---- この App が所有する State(ユーザーが操作する値)----
    Q_PROPERTY(int selectedChannel READ selectedChannel WRITE setSelectedChannel NOTIFY configChanged)   // 1..18(CH18 = 呼出チャネル)
    Q_PROPERTY(bool mute READ mute WRITE setMute NOTIFY configChanged)
    Q_PROPERTY(int standby READ standby WRITE setStandby NOTIFY configChanged)         // 0 全受信 / 1 個別待受 / 2 グループ待受(表 Annex 2-4-14)
    Q_PROPERTY(int ownMaker READ ownMaker WRITE setOwnMaker NOTIFY configChanged)      // 自局機器 ID のメーカー識別(0..127)
    Q_PROPERTY(int ownIndividual READ ownIndividual WRITE setOwnIndividual NOTIFY configChanged)   // 自局機器 ID の個別 ID(0..2097151)
    Q_PROPERTY(int ownGroup READ ownGroup WRITE setOwnGroup NOTIFY configChanged)      // 自局グループ番号(0..63、0 = すべて)
    Q_PROPERTY(QString ownId READ ownId NOTIFY configChanged)
    Q_PROPERTY(double loOffsetHz READ loOffsetHz NOTIFY configChanged)
    Q_PROPERTY(int numChannels READ numChannels CONSTANT)
    // ---- 観測値(DSP thread / 復号スレッドが更新、timer で通知)----
    Q_PROPERTY(QVariantMap call READ call NOTIFY metersChanged)          // 現在の(最後の)送信
    Q_PROPERTY(QVariantList history READ history NOTIFY metersChanged)   // 受信履歴(新しい順、100 件)
    Q_PROPERTY(QVariantMap status READ status NOTIFY metersChanged)      // 同期 / フレーム / CRC / 電力 / ずれ
    Q_PROPERTY(QVariantList pins READ pins NOTIFY metersChanged)         // 地図のピン: 最後の送信 + 履歴のほかの発信者(測位あり・日本の範囲内)
    Q_PROPERTY(double losslessDrops READ losslessDrops NOTIFY metersChanged)
    Q_PROPERTY(double dspLoad READ dspLoad NOTIFY metersChanged)
    Q_PROPERTY(double voiceDrops READ voiceDrops NOTIFY metersChanged)
    Q_PROPERTY(QString audioError READ audioError NOTIFY metersChanged)
    Q_PROPERTY(QString tuneWarning READ tuneWarning NOTIFY metersChanged)   // 録音の再生で選択チャネルが帯域の外のとき
    // ---- 保存 ----
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    Q_PROPERTY(QString saveStem READ saveStem NOTIFY savingChanged)
    Q_PROPERTY(double savedFrames READ savedFrames NOTIFY metersChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QString recordPath READ recordPath NOTIFY recordingChanged)
    Q_PROPERTY(QString recordError READ recordError NOTIFY recordingChanged)
    Q_PROPERTY(double recordSeconds READ recordSeconds NOTIFY metersChanged)
    // ---- 観測点 ----
    Q_PROPERTY(spear::appfw::ViewSource* bandView READ bandView CONSTANT)        // std_t99.band(400 kHz、選択チャネルが中心)
    Q_PROPERTY(spear::appfw::ViewSource* channelView READ channelView CONSTANT)  // std_t99.channel(100 kHz、Receiver の入力)
    Q_PROPERTY(spear::appfw::TraceSource* eye READ eye CONSTANT)
public:
    explicit StdT99App(appfw::AppInfo info, QObject* parent = nullptr);
    ~StdT99App() override;
    void configure(const QVariantMap& settings) override;
    RfConfig declare_rf_config() const override;

    int selectedChannel() const { return selected_; }
    void setSelectedChannel(int ch);
    bool mute() const { return mute_; }
    void setMute(bool m);
    int standby() const { return standby_; }
    void setStandby(int s) { standby_ = std::clamp(s, 0, 2); Q_EMIT configChanged(); }
    int ownMaker() const { return own_maker_; }
    void setOwnMaker(int v) { own_maker_ = std::clamp(v, 0, 127); Q_EMIT configChanged(); }
    int ownIndividual() const { return own_individual_; }
    void setOwnIndividual(int v) { own_individual_ = std::clamp(v, 0, 2097151); Q_EMIT configChanged(); }
    int ownGroup() const { return own_group_; }
    void setOwnGroup(int v) { own_group_ = std::clamp(v, 0, 63); Q_EMIT configChanged(); }
    QString ownId() const;
    double loOffsetHz() const { return lo_offset_hz_; }
    int numChannels() const { return std_t99::kNumChannels; }
    Q_INVOKABLE double channelFreqHz(int ch) const { return std_t99::channel_freq_hz(std::clamp(ch, 1, std_t99::kNumChannels)); }

    QVariantMap call() const;
    QVariantList history() const;
    QVariantMap status() const;
    QVariantList pins() const;
    double losslessDrops() const { return sub_ ? static_cast<double>(sub_->stats().dropped_blocks) : 0; }
    double dspLoad() const { return dsp_load_; }
    double voiceDrops() const { return voice_ ? static_cast<double>(voice_->dropped()) : 0; }
    QString audioError() const { return audio_ ? QString::fromStdString(audio_->stats().error) : QString(); }
    QString tuneWarning() const { std::lock_guard<std::mutex> lk(mu_); return tune_warning_; }

    bool saving() const { return saving_; }
    QString saveStem() const { return QString::fromStdString(saver_.stem()); }
    double savedFrames() const { return static_cast<double>(saver_.frames()); }
    Q_INVOKABLE void toggleSaving();
    bool recording() const { return recording_; }
    QString recordPath() const { return QString::fromStdString(record_path_); }
    QString recordError() const { return record_error_; }
    double recordSeconds() const;
    Q_INVOKABLE void toggleRecording();

    appfw::ViewSource* bandView() { return &band_view_; }
    appfw::ViewSource* channelView() { return &channel_view_; }
    appfw::TraceSource* eye() { return &eye_; }

    // テスト用: 内部 stream(Stream Bus なので他 consumer と同じ扱い)
    Stream<float>& audio_stream() { return *audio_stream_; }

Q_SIGNALS:
    void configChanged();
    void metersChanged();
    void savingChanged();
    void recordingChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override;

private:
    void run();
    std::unique_ptr<std_t99::Monitor> build_monitor(Core& core);
    void on_frame(const std_t99::FrameEvent& e);
    void on_audio(std::span<const float> pcm, bool play);   // 復号スレッド
    void stop_saving();
    void stop_recording();
    template <class T>
    void publish(Stream<T>& s, BlockPool& pool, std::span<const T> x, uint64_t& seq, uint64_t& index);

    std::atomic<int> selected_{std_t99::kCallingChannel};   // 既定は呼出チャネル(4.7)
    bool mute_ = false;
    std::atomic<int> standby_{0}, own_maker_{0}, own_individual_{0}, own_group_{0};
    double lo_offset_hz_ = 250e3;            // site.conf: std_t99.lo_offset_hz
    std::string audio_device_ = "default";
    std::string record_dir_ = "recordings";
    std::atomic<bool> rebuild_{true};
    double built_center_ = 0;                // Monitor を組んだときの実 LO(変われば組み直す)

    // 観測値(DSP thread → GUI)
    mutable std::mutex mu_;
    std::optional<std_t99::CallInfo> call_;
    std::deque<std_t99::CallInfo> history_;
    std::optional<bool> accepted_;
    std_t99::MonitorStatus status_;
    QString tune_warning_;
    std::atomic<double> dsp_load_{0};
    std::atomic<uint64_t> cur_generation_{0};   // DSP thread が書き、復号スレッドも読む(std_t99.audio のブロックの generation)

    appfw::ViewSource band_view_, channel_view_;
    appfw::TraceSource eye_;
    std::unique_ptr<appfw::ViewProcessor> band_vp_, channel_vp_;
    std::unique_ptr<AudioSink> audio_;
    std::optional<dsp::LevelerConfig> level_cfg_ = dsp::LevelerConfig{};   // site.conf: audio_level_dbfs
    std::unique_ptr<dsp::Leveler> leveler_;  // 復号スレッド専用。AudioSink に渡す直前に通す
    std::unique_ptr<std_t99::VoiceWorker> voice_;
    std_t99::Recorder saver_;
    std::atomic<bool> saving_{false};
    std::unique_ptr<BlockPool> band_pool_, channel_pool_, audio_pool_;
    std::unique_ptr<Stream<cf32>> band_, channel_;
    std::unique_ptr<Stream<float>> audio_stream_;
    std::shared_ptr<Subscription> sub_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    int timer_ = 0;
    uint64_t band_seq_ = 0, band_index_ = 0, chan_seq_ = 0, chan_index_ = 0, audio_seq_ = 0, audio_index_ = 0;

    // REC IQ(radio.rx をそのまま SigMF に。JR TRAIN RADIO と同じ経路)
    std::unique_ptr<SigmfRecorder> rec_;
    std::shared_ptr<Subscription> rec_sub_;
    std::thread rec_th_;
    std::atomic<bool> rec_stop_{false};
    bool recording_ = false;
    std::string record_path_;
    QString record_error_;
};

} // namespace spear::apps
