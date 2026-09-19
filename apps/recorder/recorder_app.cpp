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
    timer_ = startTimer(250);
}

void RecorderApp::on_stop() {
    stopRecording();
    if (timer_) { killTimer(timer_); timer_ = 0; }
    SpectrumApp::on_stop();
}

void RecorderApp::timerEvent(QTimerEvent*) {
    if (recording_) Q_EMIT recordingChanged();
}

void RecorderApp::record() {
    if (recording_ || !core()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    path_ = dir_ + "/rec_" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss").toStdString();
    auto& rx = core()->rx();
    rec_ = std::make_unique<SigmfRecorder>(path_, rx.meta(), core()->source().config(), &core()->events(), core()->source().lo_correction_ppm());
    if (!rec_->ok()) { last_error_ = "cannot open " + QString::fromStdString(path_); rec_.reset(); Q_EMIT recordingChanged(); return; }
    last_error_.clear();
    sub_ = rx.subscribe("recorder", DeliveryPolicy::Lossless, 256);
    stop_ = false;
    th_ = std::thread([this] {
        while (!stop_) if (auto d = sub_->pop(100ms)) rec_->write(*d);
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
