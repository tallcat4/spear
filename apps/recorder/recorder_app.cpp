#include "recorder_app.hpp"

#include <QDateTime>

#include <filesystem>

namespace spear::apps {

using namespace std::chrono_literals;

RecorderApp::RecorderApp(appfw::AppInfo info, QObject* parent) : SpectrumApp(std::move(info), parent) {}

RecorderApp::~RecorderApp() { stopRecording(); }

double RecorderApp::seconds() const {
    if (!rec_ || !core()) return 0;
    const double rate = core()->source().config().sample_rate;
    return rate > 0 ? static_cast<double>(rec_->samples_written()) / rate : 0;
}

void RecorderApp::on_start(Core& core) {
    SpectrumApp::on_start(core);
    update_disk_free();
    timer_ = startTimer(250);
}

void RecorderApp::on_stop() {
    stopRecording();
    if (timer_) { killTimer(timer_); timer_ = 0; }
    SpectrumApp::on_stop();
}

void RecorderApp::update_disk_free() {
    std::error_code ec;
    const auto sp = std::filesystem::space(dir_, ec);
    disk_free_gb_ = ec ? -1 : static_cast<double>(sp.available) / 1e9;
}

void RecorderApp::timerEvent(QTimerEvent*) {
    update_disk_free();
    if (recording_) {
        // 書き込みに失敗した(ディスクが一杯など)、または空きが少なくなったら録音を止めて理由を出す。
        // 止めずに RECORDING のままにすると、書けていないことが画面から分からない
        if (rec_ && !rec_->ok()) { stopRecording(); last_error_ = "write failed (disk full?) — recording stopped"; }
        else if (disk_free_gb_ >= 0 && disk_free_gb_ < kStopBelowGb) { stopRecording(); last_error_ = QString("disk free below %1 GB — recording stopped").arg(kStopBelowGb); }
    }
    Q_EMIT recordingChanged();
}

void RecorderApp::record() {
    if (recording_ || !core()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    update_disk_free();
    if (disk_free_gb_ >= 0 && disk_free_gb_ < kStopBelowGb) { last_error_ = QString("disk free below %1 GB — not recording").arg(kStopBelowGb); Q_EMIT recordingChanged(); return; }
    path_ = dir_ + "/rec_" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss").toStdString();
    auto& rx = core()->rx();
    rec_ = std::make_unique<SigmfRecorder>(path_, rx.meta(), core()->source().config(), &core()->events(), core()->source().lo_correction_ppm());
    if (!rec_->ok()) { last_error_ = "cannot open " + QString::fromStdString(path_); rec_.reset(); Q_EMIT recordingChanged(); return; }
    last_error_.clear();
    sub_ = rx.subscribe("recorder", DeliveryPolicy::Lossless, 256);
    stop_ = false;
    th_ = std::thread([this] {
        // 書き込みに失敗したら抜ける(後始末は GUI thread の timerEvent が stopRecording で行う)
        while (!stop_ && rec_->ok()) if (auto d = sub_->pop(100ms)) rec_->write(*d);
    });
    recording_ = true;
    Q_EMIT recordingChanged();
}

void RecorderApp::stopRecording() {
    if (!recording_) return;
    stop_ = true;
    if (th_.joinable()) th_.join();
    if (rec_) rec_->close();
    sub_.reset();
    recording_ = false;
    Q_EMIT recordingChanged();   // rec_ は残す(最終値を表示し続ける)
}

} // namespace spear::apps
