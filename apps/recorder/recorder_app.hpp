// App: IQ RECORDER — spectrum + Lossless consumer による SigMF 録音 (§8.2: 録音はただの App)。
// 検証対象(docs/apps/recorder.md): App 固有 State の所有(録音状態)、Lossless 不変条件の画面化、SigMF sidecar。
#pragma once

#include "spectrum_app.hpp"
#include "spear/core/recording.hpp"

#include <atomic>
#include <thread>

namespace spear::apps {

class RecorderApp final : public SpectrumApp {
    Q_OBJECT
    // 録音の State は App が所有する。変更は record()/stopRecording() のみ。
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QString path READ path NOTIFY recordingChanged)
    Q_PROPERTY(double samplesWritten READ samplesWritten NOTIFY recordingChanged)
    Q_PROPERTY(double seconds READ seconds NOTIFY recordingChanged)
    Q_PROPERTY(double droppedBlocks READ droppedBlocks NOTIFY recordingChanged)
    Q_PROPERTY(double bytesWritten READ bytesWritten NOTIFY recordingChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY recordingChanged)
public:
    explicit RecorderApp(appfw::AppInfo info, QObject* parent = nullptr);
    ~RecorderApp() override;
    void configure(const QVariantMap& settings) override {
        if (settings.contains("record_dir")) dir_ = settings["record_dir"].toString().toStdString();
    }

    bool recording() const { return recording_; }
    QString path() const { return QString::fromStdString(path_); }
    double samplesWritten() const { return rec_ ? static_cast<double>(rec_->samples_written()) : 0; }
    double seconds() const;
    double droppedBlocks() const { return sub_ ? static_cast<double>(sub_->stats().dropped_blocks) : 0; }
    double bytesWritten() const { return samplesWritten() * 4; }
    QString lastError() const { return last_error_; }

    Q_INVOKABLE void record();          // 新しいファイルで録音開始
    Q_INVOKABLE void stopRecording();

Q_SIGNALS:
    void recordingChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override;

private:
    std::string dir_ = "recordings", path_;
    std::unique_ptr<SigmfRecorder> rec_;
    std::shared_ptr<Subscription> sub_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    bool recording_ = false;
    QString last_error_;
    int timer_ = 0;
};

} // namespace spear::apps
