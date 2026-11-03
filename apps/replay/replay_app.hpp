// App: IQ REPLAY TX — 録音した IQ をそのまま送信する(要件 §7.2 Replay Transmitter、IQ RECORDER の逆)。
// 検証対象(docs/apps/replay.md): 送信の fan-in(Core::tx)、TX 不変条件(全 sample 送出 or Underflow event)、TX MASTER。
//
// 安全の約束:
//   * 送信するのは TRANSMIT を押した間だけ。App の起動直後・停止後・録音の終わりでは何も送らない
//     (burst の外では B210 の ATR が送信経路を切っている。b200_impl.cpp update_atrs: IDLE = STATE_RX1_OFF)。
//   * Core の TX MASTER が SAFE なら、TRANSMIT を押しても Core が拒否する(App は拒否を表示するだけ)。
//   * 送信の減衰はプロセスの起動のたびに全範囲(最小出力)から。保存しない。
//   * 送信する周波数・レートは録音の値(captures[0])そのまま。UI から LO / rate を変える手段は置かない。
#pragma once

#include "spear/appfw/app.hpp"
#include "spear/appfw/view_source.hpp"
#include "spear/core/recording.hpp"

#include <QVariantList>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace spear::apps {

class ReplayApp final : public appfw::GuiApp {
    Q_OBJECT
    Q_PROPERTY(spear::appfw::ViewSource* view READ view CONSTANT)
    // ---- ユーザーが選ぶ State(App が所有。保存する)----
    Q_PROPERTY(QString file READ file WRITE setFile NOTIFY fileChanged)        // 録音の base path(拡張子なし)
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged)           // 録音の終わりで先頭に戻って続ける(同じ burst のまま)
    Q_PROPERTY(QString txPort READ txPort WRITE setTxPort NOTIFY txPortChanged)   // TRXA / TRXB
    // ---- 選んだ録音の情報(file から導く)----
    Q_PROPERTY(bool fileValid READ fileValid NOTIFY fileChanged)
    Q_PROPERTY(QString fileError READ fileError NOTIFY fileChanged)
    Q_PROPERTY(double fileFreq READ fileFreq NOTIFY fileChanged)
    Q_PROPERTY(double fileRate READ fileRate NOTIFY fileChanged)
    Q_PROPERTY(double fileSeconds READ fileSeconds NOTIFY fileChanged)
    Q_PROPERTY(int fileDiscontinuities READ fileDiscontinuities NOTIFY fileChanged)
    Q_PROPERTY(int fileRetunes READ fileRetunes NOTIFY fileChanged)
    Q_PROPERTY(QVariantList recordings READ recordings NOTIFY recordingsChanged)   // ListEntry 用 [{label, sublabel, value}]
    // 運転中の宣言(Core)と選んだ録音・端子が一致しているか。違えば App の再起動が要る(TRANSMIT は押せない)
    Q_PROPERTY(bool configCurrent READ configCurrent NOTIFY statsChanged)
    // ---- 送信の状態 ----
    Q_PROPERTY(bool sending READ sending NOTIFY statsChanged)           // App が burst を流している(TRANSMIT 〜 STOP / 終わり)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY statsChanged)   // 録音内の位置
    Q_PROPERTY(int passes READ passes NOTIFY statsChanged)              // LOOP で先頭に戻った回数
    Q_PROPERTY(QString lastError READ lastError NOTIFY statsChanged)    // 拒否・失敗の理由(TX MASTER SAFE 等)
public:
    explicit ReplayApp(appfw::AppInfo info, QObject* parent = nullptr);
    ~ReplayApp() override;
    void configure(const QVariantMap& settings) override;
    RfConfig declare_rf_config() const override;

    appfw::ViewSource* view() { return &view_; }
    QString file() const { return file_; }
    void setFile(const QString& base);
    bool loop() const { return loop_; }
    void setLoop(bool on) { if (loop_ == on) return; loop_ = on; Q_EMIT loopChanged(); }
    QString txPort() const;
    void setTxPort(const QString& name);
    bool fileValid() const { return file_valid_; }
    QString fileError() const { return file_error_; }
    double fileFreq() const { return file_freq_; }
    double fileRate() const { return file_rate_; }
    double fileSeconds() const { return file_rate_ > 0 ? static_cast<double>(file_samples_) / file_rate_ : 0; }
    int fileDiscontinuities() const { return file_disc_; }
    int fileRetunes() const { return file_retunes_; }
    QVariantList recordings() const { return recordings_; }
    bool configCurrent() const;
    bool sending() const { return sending_.load(); }
    double positionSeconds() const { return file_rate_ > 0 ? static_cast<double>(position_.load()) / file_rate_ : 0; }
    int passes() const { return passes_.load(); }
    QString lastError() const { std::lock_guard lk(err_mu_); return last_error_; }

    Q_INVOKABLE void refreshRecordings();
    Q_INVOKABLE void transmit();       // 録音の先頭から 1 つの burst を始める
    Q_INVOKABLE void stopTransmit();   // burst を閉じる(EOB)
    // 送信の減衰 [dB](0 = 最大出力、sys.txAttenMax = 最小出力)。運転中は Core の宣言(sys.txAtten)を変える。
    // 起動前の値は App が持つ(起動時に宣言する。保存しない = プロセスの起動のたびに最小出力から)。範囲の照合は Core(Radio)
    Q_INVOKABLE void setTxAtten(double db);

Q_SIGNALS:
    void fileChanged();
    void loopChanged();
    void txPortChanged();
    void recordingsChanged();
    void statsChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override { Q_EMIT statsChanged(); }

private:
    void load_file_info();
    void run_burst();
    void join_burst();
    void set_error(const QString& e) { std::lock_guard lk(err_mu_); last_error_ = e; }

    appfw::ViewSource view_;
    std::unique_ptr<Stream<sc16>> view_stream_;    // "replay.tx": 送っている IQ(表示用 fan-out)
    std::unique_ptr<appfw::ViewProcessor> vp_;
    // 送信キューの block はこの pool を指す。Source が止まる(Core::stop_app が先に止める)まで生かす = App と同じ寿命
    BlockPool pool_;
    TxStream* tx_ = nullptr;
    std::thread burst_th_;
    std::atomic<bool> sending_{false}, stop_burst_{false};
    std::atomic<uint64_t> position_{0};
    std::atomic<int> passes_{0};
    mutable std::mutex err_mu_;
    QString last_error_;

    QString file_;
    bool loop_ = false;
    RfPort tx_port_ = RfPort::TrxA;
    double tx_atten_ = kTxAttenFullRangeDb;   // 起動時に宣言する送信の減衰(運転中の値は Core が持つ)
    QStringList dirs_;
    QVariantList recordings_;
    bool file_valid_ = false;
    QString file_error_;
    double file_freq_ = 0, file_rate_ = 0;
    uint64_t file_samples_ = 0;
    int file_disc_ = 0, file_retunes_ = 0;
    int timer_ = 0;
};

} // namespace spear::apps
