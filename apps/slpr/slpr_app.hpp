// App: SLPR 20CH RX — 特定小電力トランシーバー(422 MHz 帯、単信 20 ch)の同時受信。docs/apps/slpr.md
//
// 純 C++ の Receiver(20ch NFM)を GUI に載せる。STD-T98 MONITOR の骨格をそのまま使い、デジタル固有のもの(アイパターン、フレーム、
// CSM、秘話)を持たない分だけ簡単になっている。検証対象:
//   * 同じ骨格(resamp1 → PFB → チャネルごと)の 2 つ目の App: 共通 DSP の境界(quadrature demod / squelch は 3 度目の App 内実装)
//   * 音声ミキサの単純形: 全チャネルが同じ歩みで音声を出すので provenance で並べ直す必要が無い(STD-T98 の Mixer との対比)
//   * §3.6 provenance: squelch 開閉 event の sample 範囲を radio.rx index で
// RF: 4 Msps、LO は帯域中心(ch11 = 422.1750 MHz)から loOffset(250 kHz)だけ下(ゼロ IF の DC スパイクを帯域の外へ)。
#pragma once

#include "receiver.hpp"
#include "spear/appfw/app.hpp"
#include "spear/appfw/view_source.hpp"
#include "spear/core/audio_sink.hpp"
#include "spear/core/block.hpp"
#include "spear/core/stream.hpp"

#include <QVariantList>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace spear::apps {

class SlprApp final : public appfw::GuiApp {
    Q_OBJECT
    // ---- この App が所有する State ----
    // 帯域中心 / LO オフセットはチャネル表と App が決める(UI からは操作しない: 誤設定で受信不能になる)。個体差は Core(radio.freq_err_ppm)
    Q_PROPERTY(double bandCenterHz READ bandCenterHz NOTIFY configChanged)   // ch11 の周波数(422.1750 MHz)
    Q_PROPERTY(double loOffsetHz READ loOffsetHz NOTIFY configChanged)
    Q_PROPERTY(double squelchMarginDb READ squelchMarginDb WRITE setSquelchMarginDb NOTIFY configChanged)   // 床(全 ch 中央値)からの余裕 [dB]
    Q_PROPERTY(int selectedChannel READ selectedChannel WRITE setSelectedChannel NOTIFY configChanged)   // 0..19
    Q_PROPERTY(bool mute READ mute WRITE setMute NOTIFY configChanged)
    Q_PROPERTY(bool allChannelAudio READ allChannelAudio WRITE setAllChannelAudio NOTIFY configChanged)   // 全 20ch を同時再生(ミックス)
    Q_PROPERTY(int numChannels READ numChannels CONSTANT)
    Q_PROPERTY(double channelSpacingHz READ channelSpacingHz CONSTANT)
    // ---- 観測値(DSP thread が更新、timer で通知)----
    Q_PROPERTY(QVariantList channels READ channels NOTIFY metersChanged)   // 20 × {ch, name, freq, group, power, floor, threshold, open, audioDb, opens}
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY metersChanged)    // 選択 ch の詳細
    Q_PROPERTY(int openChannels READ openChannels NOTIFY metersChanged)
    Q_PROPERTY(double mixDb READ mixDb NOTIFY metersChanged)               // ミックス後の音声レベル
    Q_PROPERTY(double losslessDrops READ losslessDrops NOTIFY metersChanged)
    Q_PROPERTY(double dspLoad READ dspLoad NOTIFY metersChanged)           // DSP thread の実時間比
    Q_PROPERTY(QString audioError READ audioError NOTIFY metersChanged)
    Q_PROPERTY(double audioUnderruns READ audioUnderruns NOTIFY metersChanged)
    Q_PROPERTY(double audioOverruns READ audioOverruns NOTIFY metersChanged)
    Q_PROPERTY(double audioLatencyMs READ audioLatencyMs NOTIFY metersChanged)
    // ---- 観測点 ----
    Q_PROPERTY(spear::appfw::ViewSource* view READ view CONSTANT)          // radio.rx
    Q_PROPERTY(spear::appfw::ViewSource* bandView READ bandView CONSTANT)  // slpr.band(400 kHz、20ch 俯瞰)
    Q_PROPERTY(spear::appfw::ViewSource* channelView READ channelView CONSTANT)   // slpr.channel(選択 ch 12.5 kHz)
public:
    explicit SlprApp(appfw::AppInfo info, QObject* parent = nullptr);
    ~SlprApp() override;
    void configure(const QVariantMap& settings) override;
    RfConfig declare_rf_config() const override;

    double bandCenterHz() const { return band_center_hz_; }
    double loOffsetHz() const { return lo_offset_hz_; }
    double squelchMarginDb() const { return squelch_margin_db_; }
    void setSquelchMarginDb(double v) { squelch_margin_db_ = std::clamp(v, 0.0, 40.0); Q_EMIT configChanged(); }
    int selectedChannel() const { return selected_; }
    void setSelectedChannel(int ch);
    bool mute() const { return mute_; }
    void setMute(bool m);
    bool allChannelAudio() const { return all_audio_; }
    void setAllChannelAudio(bool on) { all_audio_ = on; Q_EMIT configChanged(); }
    int numChannels() const { return slpr::kNumChannels; }
    double channelSpacingHz() const { return slpr::kSpacingHz; }
    Q_INVOKABLE double channelFreqHz(int ch) const { return slpr::kChannels[static_cast<std::size_t>(std::clamp(ch, 0, slpr::kNumChannels - 1))].freq_hz; }
    Q_INVOKABLE double channelOffsetHz(int ch) const { return slpr::channel_offset_hz(std::clamp(ch, 0, slpr::kNumChannels - 1)); }
    Q_INVOKABLE QString channelName(int ch) const { return QString::fromLatin1(slpr::kChannels[static_cast<std::size_t>(std::clamp(ch, 0, slpr::kNumChannels - 1))].name); }
    Q_INVOKABLE QString channelGroup(int ch) const { return QString::fromLatin1(slpr::kChannels[static_cast<std::size_t>(std::clamp(ch, 0, slpr::kNumChannels - 1))].group); }

    QVariantList channels() const;
    QVariantMap selected() const;
    int openChannels() const;
    double mixDb() const { return mix_db_; }
    double losslessDrops() const { return sub_ ? static_cast<double>(sub_->stats().dropped_blocks) : 0; }
    double dspLoad() const { return dsp_load_; }
    QString audioError() const { return audio_ ? QString::fromStdString(audio_->stats().error) : QString(); }
    double audioUnderruns() const { return audio_ ? static_cast<double>(audio_->stats().underruns) : 0; }
    double audioOverruns() const { return audio_ ? static_cast<double>(audio_->stats().overruns) : 0; }
    double audioLatencyMs() const { return audio_ ? audio_->stats().latency_ms : 0; }
    appfw::ViewSource* view() { return &view_; }
    appfw::ViewSource* bandView() { return &band_view_; }
    appfw::ViewSource* channelView() { return &channel_view_; }

    // テスト用: 内部 stream への直接アクセス(Stream Bus なので他 consumer と同じ扱い)
    Stream<float>& audio_stream() { return *audio_stream_; }

Q_SIGNALS:
    void configChanged();
    void metersChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override;

private:
    void run();
    std::unique_ptr<slpr::Receiver> build_receiver(Core& core);
    template <class T>
    void publish(Stream<T>& s, BlockPool& pool, std::span<const T> x, uint64_t& seq, uint64_t& index);

    double band_center_hz_ = slpr::kBandCenterHz;
    double lo_offset_hz_ = 250e3;
    std::atomic<double> squelch_margin_db_{8.0};   // 床 + 8 dB で開く(ReceiverConfig の既定と同じ。site.conf: slpr.squelch_margin_db)
    std::atomic<int> selected_{0};
    bool mute_ = false;
    std::atomic<bool> all_audio_{true};
    std::string audio_device_ = "default";
    std::atomic<bool> rebuild_{true};
    double built_center_ = 0;   // Receiver を組んだときの実 LO(変われば組み直す)

    // 観測値(DSP thread → GUI)
    mutable std::mutex mu_;
    struct ChanStat { double power_db = -999, floor_db = -999, threshold_db = -999, audio_db = -999; bool open = false; uint64_t opens = 0; double open_s = 0; uint64_t open_index = 0; };
    std::vector<ChanStat> stats_;
    std::atomic<double> dsp_load_{0}, mix_db_{-999};
    uint64_t cur_generation_ = 0;   // DSP thread 専用: 処理中ブロックの generation(event の provenance 用)

    appfw::ViewSource view_, band_view_, channel_view_;
    std::unique_ptr<appfw::ViewProcessor> vp_, band_vp_, channel_vp_;
    std::unique_ptr<AudioSink> audio_;
    // 音声ミキサ(DSP thread 専用): 全チャネルの音声は同じブロックで同じ数だけ出るので、加算するだけ(provenance で並べ直す必要が無い)
    std::vector<float> mix_;
    std::unique_ptr<BlockPool> band_pool_, channel_pool_, audio_pool_;
    std::unique_ptr<Stream<cf32>> band_, channel_;
    std::unique_ptr<Stream<float>> audio_stream_;
    std::shared_ptr<Subscription> sub_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    int timer_ = 0;
    uint64_t band_seq_ = 0, band_index_ = 0, chan_seq_ = 0, chan_index_ = 0, audio_seq_ = 0, audio_index_ = 0;
};

} // namespace spear::apps
