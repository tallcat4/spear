// spear-gui — SystemVolume: OS(PipeWire)の出力の音量を読み書きする。docs/audio.md
//
// 音量の持ち主は OS の既定の出力(@DEFAULT_AUDIO_SINK@)ただ 1 つ。spear は音量を持たず、wpctl で読み書きするだけ
// (Plasma の音量や他のアプリと同じ値。Plasma に戻っても同じ音量)。値は操作のたびに OS から読み直して表示する。
// 操作は順に 1 つずつ実行する(取りこぼさない。wpctl は数 ms)。上限は 100 %(PipeWire の増幅はしない)。
// 音量キー(Qt::Key_VolumeUp / Down / Mute)をアプリ全体のイベントフィルタで拾う。キオスク(cage)では OS が扱わないため。
// Plasma の中ではキーは KWin が先に取り、Plasma の表示が出る(ここには来ない)。
#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>

#include <deque>

namespace spear::gui {

class SystemVolume : public QObject {
    Q_OBJECT
    Q_PROPERTY(double volume READ volume NOTIFY changed)       // 0..1
    Q_PROPERTY(bool muted READ muted NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)   // wpctl で読めた
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    static constexpr double kStep = 0.05;

    explicit SystemVolume(QObject* parent = nullptr);
    double volume() const { return volume_; }
    bool muted() const { return muted_; }
    bool available() const { return available_; }
    QString error() const { return error_; }

    Q_INVOKABLE void step(int direction);   // +1 / −1(kStep ずつ。ミュートは解く)
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void changed();
    void keyUsed();   // 音量キーが押された(QML は表示を出す)

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void enqueue(QStringList args);
    void run_next();
    void finished(int code, QProcess::ExitStatus status);

    QProcess proc_;
    std::deque<QStringList> queue_;
    bool reading_ = false;   // 実行中のものが get-volume か
    double volume_ = 0;
    bool muted_ = false, available_ = false;
    QString error_;
};

} // namespace spear::gui
