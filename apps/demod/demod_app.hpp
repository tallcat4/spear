// App: FM / AM RX — dogfooding (docs/apps/demod.md)
//
// 検証対象:
//   * DSP 境界 (docs/dsp-boundary.md): 共通は FIR 設計 / FirDecimator のみ。quadrature_demod / magnitude /
//     de-emphasis / squelch は App 内(2 つ目の App が要ったら抽出)
//   * 段の規約 + Provenance: 搬送波検出 event の sample 範囲を入力 index に逆算
//   * §0: App 内部の channel IQ と audio を Stream Bus に publish し、waterfall を DSP に触れず後付け
//   * AudioSink (ALSA): underrun を数え event に
// レート: 1.92 Msps (B210 の 30.72 MHz / 16) → /8 = 240 kHz channel → /5 = 48 kHz audio
#pragma once

#include "spear/appfw/app.hpp"
#include "spear/appfw/view_source.hpp"
#include "spear/core/audio_sink.hpp"
#include "spear/core/block.hpp"
#include "spear/core/stream.hpp"
#include "spear/dsp/fir.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace spear::apps {

class DemodApp final : public appfw::GuiApp {
    Q_OBJECT
    // ---- この App が所有する State(変更は setter のみ)----
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)          // "WFM" | "NFM" | "AM"
    Q_PROPERTY(double channelOffsetHz READ channelOffsetHz WRITE setChannelOffsetHz NOTIFY modeChanged)
    Q_PROPERTY(double squelchDb READ squelchDb WRITE setSquelchDb NOTIFY modeChanged)
    Q_PROPERTY(bool mute READ mute WRITE setMute NOTIFY audioChanged)
    // ---- 観測値(App の DSP thread が更新)----
    Q_PROPERTY(bool squelchOpen READ squelchOpen NOTIFY metersChanged)
    Q_PROPERTY(double signalDb READ signalDb NOTIFY metersChanged)
    Q_PROPERTY(double audioDb READ audioDb NOTIFY metersChanged)
    Q_PROPERTY(double audioUnderruns READ audioUnderruns NOTIFY metersChanged)
    Q_PROPERTY(double audioOverruns READ audioOverruns NOTIFY metersChanged)
    Q_PROPERTY(double audioLatencyMs READ audioLatencyMs NOTIFY metersChanged)
    Q_PROPERTY(QString audioError READ audioError NOTIFY metersChanged)
    Q_PROPERTY(double channelBandwidth READ channelBandwidth NOTIFY modeChanged)
    Q_PROPERTY(double dspDelayMs READ dspDelayMs NOTIFY modeChanged)              // Provenance の群遅延合計
    Q_PROPERTY(double lossless_drops READ losslessDrops NOTIFY metersChanged)
    // ---- 観測点(Stream Bus 経由。DSP には触れない)----
    Q_PROPERTY(spear::appfw::ViewSource* view READ view CONSTANT)          // radio.rx
    Q_PROPERTY(spear::appfw::ViewSource* channelView READ channelView CONSTANT)   // demod.channel
public:
    explicit DemodApp(appfw::AppInfo info, QObject* parent = nullptr);
    ~DemodApp() override;
    void configure(const QVariantMap& settings) override;
    RfConfig declare_rf_config() const override;   // rate は App が決める(1.92 Msps)

    QString mode() const { return QString::fromStdString(mode_); }
    void setMode(const QString& m);
    double channelOffsetHz() const { return offset_hz_; }
    void setChannelOffsetHz(double hz) { offset_hz_ = hz; rebuild_ = true; Q_EMIT modeChanged(); }
    double squelchDb() const { return squelch_db_; }
    void setSquelchDb(double v) { squelch_db_ = v; Q_EMIT modeChanged(); }
    bool mute() const { return mute_; }
    void setMute(bool m);
    bool squelchOpen() const { return squelch_open_; }
    double signalDb() const { return signal_db_; }
    double audioDb() const { return audio_db_; }
    double audioUnderruns() const { return audio_ ? static_cast<double>(audio_->stats().underruns) : 0; }
    double audioOverruns() const { return audio_ ? static_cast<double>(audio_->stats().overruns) : 0; }
    double audioLatencyMs() const { return audio_ ? audio_->stats().latency_ms : 0; }
    QString audioError() const { return audio_ ? QString::fromStdString(audio_->stats().error) : QString(); }
    double channelBandwidth() const;
    double dspDelayMs() const { return dsp_delay_ms_; }
    double losslessDrops() const { return sub_ ? static_cast<double>(sub_->stats().dropped_blocks) : 0; }
    appfw::ViewSource* view() { return &view_; }
    appfw::ViewSource* channelView() { return &channel_view_; }

    // ---- 選局 ----
    // ゼロ IF の DC スパイク / LO 漏れを避けるため、復調チャネルは LO(Core の center)から loOffset だけ離す。
    // RX 周波数(ユーザーの主値)= center + channelOffset。App は center = rx − loOffset を Core に要求し、
    // channelOffset = loOffset で mixer が戻す。RX 周波数は導出値であり、コピーは持たない(state-ownership)。
    Q_PROPERTY(double loOffsetHz READ loOffsetHz WRITE setLoOffsetHz NOTIFY modeChanged)
    double loOffsetHz() const { return lo_offset_hz_; }
    void setLoOffsetHz(double hz);
    Q_INVOKABLE double rxFreq() const;                 // 導出: center + channelOffset
    Q_INVOKABLE void tuneRx(double rx_hz);             // RX 周波数を指定(LO は自動で離れる)
    Q_INVOKABLE void stepRx(double d) { tuneRx(rxFreq() + d); }
    Q_INVOKABLE void cycleMode();

    // テスト用: 内部 stream への直接アクセス(Stream Bus なので他 consumer と同じ扱い)
    Stream<cf32>& channel_stream() { return *channel_; }
    Stream<float>& audio_stream() { return *audio_stream_; }

Q_SIGNALS:
    void modeChanged();
    void audioChanged();
    void metersChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override;

private:
    struct Chain;                       // DSP 段(mode ごとに再構築)
    void run();
    std::unique_ptr<Chain> build_chain(double in_rate);

    std::string mode_ = "NFM";
    std::atomic<double> offset_hz_{250e3};
    double lo_offset_hz_ = 250e3;
    std::atomic<double> squelch_db_{-70.0};
    bool mute_ = false;
    std::string audio_device_ = "default";
    std::atomic<bool> rebuild_{true};

    std::atomic<bool> squelch_open_{false};
    std::atomic<double> signal_db_{-999}, audio_db_{-999};
    double dsp_delay_ms_ = 0;

    appfw::ViewSource view_, channel_view_;
    std::unique_ptr<appfw::ViewProcessor> vp_, channel_vp_;
    std::unique_ptr<AudioSink> audio_;
    std::unique_ptr<BlockPool> channel_pool_, audio_pool_;
    std::unique_ptr<Stream<cf32>> channel_;
    std::unique_ptr<Stream<float>> audio_stream_;
    std::shared_ptr<Subscription> sub_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    int timer_ = 0;
    uint64_t chan_seq_ = 0, chan_index_ = 0, audio_seq_ = 0, audio_index_ = 0;
};

} // namespace spear::apps
