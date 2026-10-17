// App: JR TRAIN RADIO — JR 在来線デジタル列車無線(352.6 MHz 帯、π/4-DQPSK 4800 baud)の多波同時受信。docs/apps/jr_train.md
//
//   * 全チャネルを同時に復調・復号する(フレーム同期、音声の誤り訂正と合成、データ電文、運行把握)
//   * 音声は 1 チャネルを再生する。AUTO: 最初に始まった通話を選び、終われば続いている通話のうち開始が早いものへ(原本と同じ規則)。
//     タイルを手で選ぶと選択を固定する(AUTO が外れる)
//   * 画面下: 選択チャネルのコンスタレーション / 受信データ(電文)/ 運行把握(VIEW で切り替え)
//   * SAVE: 復号した音声(チャネル別 WAV)・生フレーム・電文(JSONL)を保存する(原本と同じ形式)
//   * REC IQ: radio.rx(1.92 MS/s ci16)を SigMF で録音する(golden の収集。spear-jr-train-decode で再現できる)
// RF: 1.92 MS/s、LO は帯域中心から loOffset だけ下げる(ゼロ IF の DC を帯域から外す)。帯域は Receiver の回転で戻す。
// プリセット(east / central / west)は site.conf の jr_train.preset。画面に周波数・レートの操作は置かない。
#pragma once

#include "call_monitor.hpp"
#include "presets.hpp"
#include "receiver.hpp"
#include "storage.hpp"
#include "spear/appfw/app.hpp"
#include "spear/appfw/view_source.hpp"
#include "spear/core/audio_sink.hpp"
#include "spear/core/block.hpp"
#include "spear/core/recording.hpp"
#include "spear/core/stream.hpp"

#include <QVariantList>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace spear::apps {

class JrTrainApp final : public appfw::GuiApp {
    Q_OBJECT
    // ---- この App が所有する State ----
    Q_PROPERTY(QString presetId READ presetId NOTIFY configChanged)          // site.conf: jr_train.preset
    Q_PROPERTY(QString presetLabel READ presetLabel NOTIFY configChanged)
    Q_PROPERTY(double bandCenterHz READ bandCenterHz NOTIFY configChanged)
    Q_PROPERTY(double loOffsetHz READ loOffsetHz NOTIFY configChanged)
    Q_PROPERTY(int numChannels READ numChannels NOTIFY routeSelectionChanged)
    Q_PROPERTY(int selectedChannel READ selectedChannel WRITE setSelectedChannel NOTIFY selectedChannelChanged)
    Q_PROPERTY(bool autoSelect READ autoSelect WRITE setAutoSelect NOTIFY autoSelectChanged)      // 通話を自動選択
    Q_PROPERTY(bool mute READ mute WRITE setMute NOTIFY muteChanged)
    Q_PROPERTY(QString lowerView READ lowerView WRITE setLowerView NOTIFY lowerViewChanged)      // "const" / "data" / "ops"
    // 路線から受信するチャネルを選ぶ(原本の「路線から選ぶ」)。空 = プリセットの全チャネル
    Q_PROPERTY(QString routeSelection READ routeSelection WRITE setRouteSelection NOTIFY routeSelectionChanged)   // 路線 ID をコンマ区切り
    Q_PROPERTY(bool includeCommon READ includeCommon WRITE setIncludeCommon NOTIFY routeSelectionChanged)         // 路線選択時も共用波を受信
    Q_PROPERTY(QVariantList routes READ routes NOTIFY configChanged)              // プリセットの路線 {id, line, section, designation, voice1, voice2, data, note}
    Q_PROPERTY(QString receiveSet READ receiveSet NOTIFY routeSelectionChanged)    // 受信対象の説明("ALL 22 ch" / "ROUTES 3 → 7 ch")
    Q_PROPERTY(QString presetError READ presetError NOTIFY configChanged)         // 外部プリセットファイルの読み込みエラー(組み込みに戻した)
    // ---- 観測値(DSP thread が更新、timer で通知)----
    Q_PROPERTY(QVariantList channels READ channels NOTIFY metersChanged)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY metersChanged)
    Q_PROPERTY(QVariantList constellation READ constellation NOTIFY metersChanged)   // 選択チャネル [x0, y0, x1, y1, ...](RMS 正規化)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY metersChanged)             // 選択チャネルの電文(新しい順)
    Q_PROPERTY(QVariantMap operation READ operation NOTIFY metersChanged)            // 運行把握(最後に完成した表)
    Q_PROPERTY(int monitoredChannel READ monitoredChannel NOTIFY metersChanged)      // 再生中のチャネル(−1 = なし)
    Q_PROPERTY(QVariantList activeCalls READ activeCalls NOTIFY metersChanged)       // 通話中のチャネル(始まりの順)
    Q_PROPERTY(int activeChannels READ activeChannels NOTIFY metersChanged)          // 直近 1 s にフレーム同期したチャネル数
    Q_PROPERTY(double losslessDrops READ losslessDrops NOTIFY metersChanged)
    Q_PROPERTY(double discontinuities READ discontinuities NOTIFY metersChanged)
    Q_PROPERTY(double dspLoad READ dspLoad NOTIFY metersChanged)                     // DSP の実時間比(前段 + 全チャネル、壁時計)
    Q_PROPERTY(int dspThreads READ dspThreads NOTIFY metersChanged)
    Q_PROPERTY(QString audioError READ audioError NOTIFY metersChanged)
    Q_PROPERTY(double audioUnderruns READ audioUnderruns NOTIFY metersChanged)
    // ---- 保存(復号結果)----
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    Q_PROPERTY(QVariantMap saveStatus READ saveStatus NOTIFY savingChanged)
    // ---- 録音(IQ、golden の収集)----
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(double recordSeconds READ recordSeconds NOTIFY recordingChanged)
    Q_PROPERTY(QString recordPath READ recordPath NOTIFY recordingChanged)
    Q_PROPERTY(QString recordError READ recordError NOTIFY recordingChanged)
    // ---- 観測点 ----
    Q_PROPERTY(spear::appfw::ViewSource* view READ view CONSTANT)           // radio.rx
    Q_PROPERTY(spear::appfw::ViewSource* bandView READ bandView CONSTANT)   // jr_train.band(回転・間引き後 240 kS/s)
public:
    explicit JrTrainApp(appfw::AppInfo info, QObject* parent = nullptr);
    ~JrTrainApp() override;
    void configure(const QVariantMap& settings) override;
    RfConfig declare_rf_config() const override;

    QString presetId() const { return QString::fromStdString(preset_->id); }
    QString presetLabel() const { return QString::fromStdString(preset_->label); }
    double bandCenterHz() const { return preset_->center_hz; }
    double loOffsetHz() const { return lo_offset_hz_; }
    int numChannels() const;
    QString routeSelection() const { return route_selection_; }
    void setRouteSelection(const QString& ids);
    bool includeCommon() const { return include_common_; }
    void setIncludeCommon(bool on);
    QVariantList routes() const;
    QString receiveSet() const;
    QString presetError() const { return preset_error_; }
    Q_INVOKABLE void applyRoutes(const QStringList& ids, bool includeCommon);   // 路線の選択を確定(受信をやり直す)
    Q_INVOKABLE QVariantList previewRoutes(const QStringList& ids, bool includeCommon) const;   // 確定前に受信する周波数を確かめる
    int selectedChannel() const { return selected_; }
    void setSelectedChannel(int ch);
    Q_INVOKABLE void selectChannelByTouch(int ch);   // 手で選ぶ: 選択を固定し AUTO を外す
    bool autoSelect() const { return auto_select_; }
    void setAutoSelect(bool on);
    bool mute() const { return mute_; }
    void setMute(bool m);
    QString lowerView() const { return lower_view_; }
    void setLowerView(const QString& v);
    Q_INVOKABLE double channelOffsetHz(int ch) const;

    QVariantList channels() const;
    QVariantMap selected() const;
    QVariantList constellation() const;
    QVariantList messages() const;
    QVariantMap operation() const;
    int monitoredChannel() const { return monitored_; }
    QVariantList activeCalls() const;
    int activeChannels() const;
    double losslessDrops() const { return sub_ ? static_cast<double>(sub_->stats().dropped_blocks) : 0; }
    double discontinuities() const { return static_cast<double>(discontinuities_.load()); }
    double dspLoad() const { return dsp_load_; }
    int dspThreads() const { return dsp_threads_; }
    QString audioError() const { return audio_ ? QString::fromStdString(audio_->stats().error) : QString(); }
    double audioUnderruns() const { return audio_ ? static_cast<double>(audio_->stats().underruns) : 0; }

    bool saving() const;
    QVariantMap saveStatus() const;
    Q_INVOKABLE void toggleSaving();

    bool recording() const { return recording_; }
    double recordSeconds() const;
    QString recordPath() const { return QString::fromStdString(record_path_); }
    QString recordError() const { return record_error_; }
    Q_INVOKABLE void toggleRecording();

    appfw::ViewSource* view() { return &view_; }
    appfw::ViewSource* bandView() { return &band_view_; }

Q_SIGNALS:
    void configChanged();
    void selectedChannelChanged();
    void autoSelectChanged();
    void muteChanged();
    void lowerViewChanged();
    void routeSelectionChanged();
    void metersChanged();
    void recordingChanged();
    void savingChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override;

private:
    void run();
    std::unique_ptr<jr_train::Receiver> build_receiver(Core& core);
    void publish_band(std::span<const cf32> x, uint64_t first_index);
    void deliver_audio(jr_train::Receiver& rx);
    void refresh_display(jr_train::Receiver& rx);
    void startRecording();
    void stopRecording();
    std::vector<jr_train::ChannelConfig> channel_configs() const;

    void recompute_active();
    std::vector<jr_train::Preset> catalog_;   // 組み込み、または site.conf の jr_train.presets_file
    const jr_train::Preset* preset_ = nullptr;   // catalog_ の要素
    std::vector<jr_train::ChannelDef> active_;   // 受信するチャネル(mu_ で保護。路線選択で変わる)
    QString route_selection_;
    bool include_common_ = true;
    QString preset_error_;
    std::atomic<uint64_t> layout_{0};            // active_ を変えるたびに増やす(DSP thread が受信機を組み直す)
    uint64_t built_layout_ = UINT64_MAX;         // DSP thread 専用
    std::vector<jr_train::ChannelDef> rx_channels_;   // DSP thread 専用: 組んだ受信機のチャネル
    double lo_offset_hz_ = 250e3;
    int threads_ = 0;                     // site.conf: jr_train.threads(0 = 自動)
    std::string record_dir_ = "recordings";
    std::string audio_device_ = "default";
    jr_train::OutputOptions save_options_{true, true, true};   // site.conf: jr_train.save_audio / save_raw / save_messages
    std::atomic<int> selected_{0};
    std::atomic<bool> auto_select_{true};
    bool mute_ = false;
    QString lower_view_ = "const";
    double built_center_ = 0;             // Receiver を組んだときの実 LO

    // ---- DSP thread 専用 ----
    struct ChanAudio {
        std::deque<std::pair<uint64_t, jr_train::Audio>> queue;   // (時刻 µs, 音声)。最大 125 フレーム(5 s)
        uint64_t audio_origin_us = 0;     // フレーム index 0 の時刻(最後のフレームから逆算)
        uint64_t epoch = 0;
        bool monitored = false;
        std::vector<jr_train::CallMonitor::Activity> activity;
        bool have_epoch = false;
        uint64_t frame_epoch = 0;
    };
    std::vector<ChanAudio> chan_audio_;
    std::unique_ptr<jr_train::CallMonitor> monitor_;
    bool audio_primed_ = false;

    // ---- 観測値(DSP thread → GUI)----
    mutable std::mutex mu_;
    struct ChanStat {
        jr_train::PipelineStatus st;      // messages / constellation は持たない(選択チャネルだけ別に)
        uint64_t last_sync_index = 0;     // 最後に同期したフレームの radio.rx index
        bool ever_synced = false;
        uint64_t resets = 0, messages = 0;
        bool call = false;
    };
    std::vector<ChanStat> stats_;
    std::vector<float> constellation_;
    std::vector<jr_train::Received> selected_messages_;
    std::optional<jr_train::protocol::OperationReception> operation_;
    std::vector<int> active_calls_;
    std::atomic<int> monitored_{-1};
    uint64_t reached_index_ = 0;          // 処理済みの radio.rx index
    double in_rate_ = 1;
    std::atomic<uint64_t> discontinuities_{0};
    std::atomic<double> dsp_load_{0};
    std::atomic<int> dsp_threads_{0};

    appfw::ViewSource view_, band_view_;
    std::unique_ptr<appfw::ViewProcessor> vp_, band_vp_;
    std::unique_ptr<BlockPool> band_pool_;
    std::unique_ptr<Stream<cf32>> band_;
    uint64_t band_seq_ = 0, band_gen_ = 0;
    std::shared_ptr<Subscription> sub_;
    std::unique_ptr<AudioSink> audio_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    int timer_ = 0;

    // 保存(復号結果)。Recorder 自身がスレッド安全(DSP thread が push、GUI thread が start / stop)
    jr_train::Recorder saver_;
    std::atomic<bool> saving_{false};
    std::atomic<uint64_t> saver_layout_{UINT64_MAX};   // 保存を始めたときの layout_。組み直す前の受信機のフレームは保存に渡さない(保存対象外で止まる)
    void start_saving();

    // 録音(IQ。専用の Lossless 購読とスレッド。DSP thread とは独立)
    std::unique_ptr<SigmfRecorder> rec_;
    std::shared_ptr<Subscription> rec_sub_;
    std::thread rec_th_;
    std::atomic<bool> rec_stop_{false};
    bool recording_ = false;
    std::string record_path_;
    QString record_error_;
};

} // namespace spear::apps
