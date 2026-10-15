// App: JR TRAIN RADIO — JR 在来線デジタル列車無線(352.6 MHz 帯、π/4-DQPSK 4800 baud)の多波同時受信。docs/apps/jr_train.md
//
// 現段階(実機なしで移植した DSP の実機確認用):
//   * 全チャネルを同時に復調し、搬送波ロック・周波数誤差・EVM・フレーム同期をタイルで示す
//   * 選択チャネルのコンスタレーション(搬送波・タイミング・生き残り系列の伝送路で補正した差動シンボル)
//   * REC: radio.rx(1.92 MS/s ci16)を SigMF で録音する(golden の収集。spear-jr-train-decode で再現できる)
// 音声・データ電文・運行把握は次の段階(原本 receiver-core / jr-voice の移植)。
// RF: 1.92 MS/s、LO は帯域中心から loOffset だけ下げる(ゼロ IF の DC を帯域から外す)。帯域は Receiver の回転で戻す。
// プリセット(east / central / west)は site.conf の jr_train.preset。画面に周波数・レートの操作は置かない。
#pragma once

#include "presets.hpp"
#include "receiver.hpp"
#include "spear/appfw/app.hpp"
#include "spear/appfw/view_source.hpp"
#include "spear/core/block.hpp"
#include "spear/core/recording.hpp"
#include "spear/core/stream.hpp"

#include <QVariantList>

#include <atomic>
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
    Q_PROPERTY(int numChannels READ numChannels NOTIFY configChanged)
    Q_PROPERTY(int selectedChannel READ selectedChannel WRITE setSelectedChannel NOTIFY selectedChannelChanged)
    // ---- 観測値(DSP thread が更新、timer で通知)----
    Q_PROPERTY(QVariantList channels READ channels NOTIFY metersChanged)    // 各 {label, name, freq, offset, service, carrier, freqErr, evm, frames, synced, locked, active, epoch}
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY metersChanged)
    Q_PROPERTY(QVariantList constellation READ constellation NOTIFY metersChanged)   // 選択チャネル [x0, y0, x1, y1, ...](RMS 正規化)
    Q_PROPERTY(int activeChannels READ activeChannels NOTIFY metersChanged)  // 直近 1 s にフレーム同期したチャネル数
    Q_PROPERTY(double losslessDrops READ losslessDrops NOTIFY metersChanged)
    Q_PROPERTY(double discontinuities READ discontinuities NOTIFY metersChanged)
    Q_PROPERTY(double dspLoad READ dspLoad NOTIFY metersChanged)            // DSP の実時間比(前段 + 全チャネル、壁時計)
    Q_PROPERTY(int dspThreads READ dspThreads NOTIFY metersChanged)
    // ---- 録音(golden の収集)----
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
    int numChannels() const { return static_cast<int>(preset_->channels.size()); }
    int selectedChannel() const { return selected_; }
    void setSelectedChannel(int ch);
    Q_INVOKABLE double channelOffsetHz(int ch) const;

    QVariantList channels() const;
    QVariantMap selected() const;
    QVariantList constellation() const;
    int activeChannels() const;
    double losslessDrops() const { return sub_ ? static_cast<double>(sub_->stats().dropped_blocks) : 0; }
    double discontinuities() const { return static_cast<double>(discontinuities_.load()); }
    double dspLoad() const { return dsp_load_; }
    int dspThreads() const { return dsp_threads_; }

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
    void metersChanged();
    void recordingChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override;

private:
    void run();
    std::unique_ptr<jr_train::Receiver> build_receiver(Core& core);
    void publish_band(std::span<const cf32> x, uint64_t first_index);
    void startRecording();
    void stopRecording();

    const jr_train::Preset* preset_ = nullptr;
    double lo_offset_hz_ = 250e3;
    int threads_ = 0;                     // site.conf: jr_train.threads(0 = 自動)
    std::string record_dir_ = "recordings";
    std::atomic<int> selected_{0};
    double built_center_ = 0;             // Receiver を組んだときの実 LO

    // 観測値(DSP thread → GUI)
    mutable std::mutex mu_;
    struct ChanStat {
        jr_train::ChannelStatus st;
        uint64_t last_sync_index = 0;     // 最後に同期したフレームの radio.rx index
        bool ever_synced = false;
    };
    std::vector<ChanStat> stats_;
    std::vector<float> constellation_;
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
    std::thread th_;
    std::atomic<bool> stop_{false};
    int timer_ = 0;

    // 録音(専用の Lossless 購読とスレッド。DSP thread とは独立)
    std::unique_ptr<SigmfRecorder> rec_;
    std::shared_ptr<Subscription> rec_sub_;
    std::thread rec_th_;
    std::atomic<bool> rec_stop_{false};
    bool recording_ = false;
    std::string record_path_;
    QString record_error_;
};

} // namespace spear::apps
