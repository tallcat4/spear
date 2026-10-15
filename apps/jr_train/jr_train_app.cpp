#include "jr_train_app.hpp"
#include "spear/core/tap.hpp"

#include <QDateTime>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace spear::apps {

using namespace std::chrono_literals;
using namespace spear::jr_train;

namespace {
constexpr double kRfRate = 1.92e6;          // ÷8 で原本の 240 kS/s。B210 の master clock(30.72 MHz ÷ 16)で正確に出る
constexpr double kActiveSeconds = 1.0;      // 直近この時間内にフレーム同期したチャネルを「受信中」とする
constexpr std::size_t kConstellationPoints = 256;
}

JrTrainApp::JrTrainApp(appfw::AppInfo info, QObject* parent) : appfw::GuiApp(std::move(info), parent) {
    preset_ = find_preset("east");
    stats_.resize(preset_->channels.size());
    // 再起動をまたいで残す運転状態(プリセット・LO オフセットは site.conf、観測値は宣言しない)
    persist({"selectedChannel", "bandView.dbMin", "bandView.dbMax", "bandView.manualRange", "bandView.averaging"});
}

JrTrainApp::~JrTrainApp() { stopRecording(); }

void JrTrainApp::configure(const QVariantMap& settings) {
    if (settings.contains("jr_train.preset")) {
        if (const auto* p = find_preset(settings["jr_train.preset"].toString().toStdString())) preset_ = p;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.assign(preset_->channels.size(), {});
    }
    if (settings.contains("jr_train.lo_offset_hz")) lo_offset_hz_ = std::clamp(settings["jr_train.lo_offset_hz"].toDouble(), -800e3, 800e3);
    if (settings.contains("jr_train.threads")) threads_ = settings["jr_train.threads"].toInt();
    if (settings.contains("record_dir")) record_dir_ = settings["record_dir"].toString().toStdString();
    selected_ = std::clamp(selected_.load(), 0, numChannels() - 1);
    Q_EMIT configChanged();
}

RfConfig JrTrainApp::declare_rf_config() const {
    RfConfig c = GuiApp::declare_rf_config();   // 受信端子・利得はシェルの草案のまま
    c.sample_rate = kRfRate;
    c.bandwidth = kRfRate;
    c.center_freq = preset_->center_hz - lo_offset_hz_;
    return c;
}

double JrTrainApp::channelOffsetHz(int ch) const {
    if (ch < 0 || ch >= numChannels()) return 0;
    return preset_->channels[static_cast<std::size_t>(ch)].frequency_hz - preset_->center_hz;
}

void JrTrainApp::setSelectedChannel(int ch) {
    ch = std::clamp(ch, 0, numChannels() - 1);
    if (ch == selected_) return;
    selected_ = ch;
    {
        std::lock_guard<std::mutex> lk(mu_);
        constellation_.clear();
    }
    Q_EMIT selectedChannelChanged();
}

QVariantList JrTrainApp::channels() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantList out;
    const auto active_after = reached_index_ > static_cast<uint64_t>(kActiveSeconds * in_rate_) ? reached_index_ - static_cast<uint64_t>(kActiveSeconds * in_rate_) : 0;
    for (std::size_t i = 0; i < preset_->channels.size() && i < stats_.size(); ++i) {
        const auto& def = preset_->channels[i];
        const auto& s = stats_[i];
        QVariantMap m;
        m["label"] = QString::fromStdString(def.short_label);
        m["name"] = QString::fromStdString(def.label);
        m["freq"] = def.frequency_hz;
        m["offset"] = def.frequency_hz - preset_->center_hz;
        m["service"] = QString::fromLatin1(to_string(def.service));
        m["carrier"] = s.st.snap.carrier_locked != 0;
        m["freqErr"] = s.st.snap.frequency_offset_hz;
        m["evm"] = s.st.evm_percent;
        m["frames"] = static_cast<double>(s.st.frames);
        m["synced"] = static_cast<double>(s.st.synchronized_frames);
        m["locked"] = s.st.frame_locked;
        m["active"] = s.ever_synced && s.last_sync_index >= active_after;
        m["epoch"] = static_cast<double>(s.st.epoch);
        out.push_back(m);
    }
    return out;
}

QVariantMap JrTrainApp::selected() const {
    const auto list = channels();
    const int sel = selected_;
    QVariantMap m = sel < list.size() ? list[sel].toMap() : QVariantMap{};
    std::lock_guard<std::mutex> lk(mu_);
    if (sel < static_cast<int>(stats_.size())) {
        const auto& s = stats_[static_cast<std::size_t>(sel)];
        m["symbols"] = static_cast<double>(s.st.snap.symbols);
        m["lockedSymbols"] = static_cast<double>(s.st.snap.locked_symbols);
        m["carrierError"] = s.st.snap.carrier_error;
        m["resets"] = static_cast<double>(s.st.resets);
        m["note"] = QString::fromStdString(preset_->channels[static_cast<std::size_t>(sel)].note);
    }
    m["index"] = sel;
    return m;
}

QVariantList JrTrainApp::constellation() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantList out;
    out.reserve(static_cast<qsizetype>(constellation_.size()));
    for (float v : constellation_) out.push_back(v);
    return out;
}

int JrTrainApp::activeChannels() const {
    int n = 0;
    for (const auto& c : channels()) n += c.toMap()["active"].toBool() ? 1 : 0;
    return n;
}

std::unique_ptr<Receiver> JrTrainApp::build_receiver(Core& core) {
    // 帯域中心を DC に戻す回転 = 帯域中心 − 実際の LO。実際の LO は Core(単一の所有者)から読む:
    // B210 なら center − loOffset、録音再生なら録音時の中心。個体の LO 誤差は Core の Radio が radio.freq_err_ppm で打ち消す
    const auto& sc = core.source().config();
    built_center_ = sc.center_freq;
    ReceiverConfig c;
    c.in_rate = sc.sample_rate > 0 ? sc.sample_rate : kRfRate;
    c.rotate_hz = preset_->center_hz - built_center_;
    c.threads = threads_;
    for (const auto& ch : preset_->channels) c.channel_offsets_hz.push_back(ch.frequency_hz - preset_->center_hz);
    auto rx = std::make_unique<Receiver>(c);
    in_rate_ = c.in_rate;
    dsp_threads_ = rx->threads();
    Observer o;
    o.band = [this](std::span<const cf32> iq, uint64_t first) {
        TAP("jr_train.band", iq.data(), iq.size());
        publish_band(iq, first);
    };
    o.frame = [this, &core](const Frame& f) {
        if (!f.sync_matched) return;
        bool first_sync = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto& s = stats_[static_cast<std::size_t>(f.channel)];
            first_sync = !s.ever_synced;
            s.last_sync_index = f.input_sample_index;
            s.ever_synced = true;
        }
        // 全フレームを event にすると多すぎる(22 波 × 25 /s)。チャネルごとの最初の同期だけ記録する
        if (first_sync) {
            const auto& def = preset_->channels[static_cast<std::size_t>(f.channel)];
            const uint64_t span = static_cast<uint64_t>(protocol::kFrameBits / 2 * in_rate_ / jr_train::dsp::kSymbolRate);
            core.events().emit(EventKind::Info, info().id, "frame sync " + def.short_label + " " + std::to_string(def.frequency_hz / 1e6).substr(0, 9) + " MHz",
                               {0, f.input_sample_index, f.input_sample_index + span}, f.channel);
        }
    };
    rx->set_observer(std::move(o));
    return rx;
}

void JrTrainApp::publish_band(std::span<const cf32> x, uint64_t first_index) {
    if (!band_ || !band_pool_) return;
    std::size_t off = 0;
    const std::size_t cap = band_pool_->slot_bytes() / sizeof(cf32);
    while (off < x.size()) {
        const std::size_t n = std::min(cap, x.size() - off);
        auto b = band_pool_->acquire<cf32>(n);
        std::copy_n(x.data() + off, n, b.data<cf32>().data());
        auto& h = b.header();
        h.generation = band_gen_;
        h.sequence = band_seq_++;
        h.sample_index = first_index + off;
        band_->publish(b.commit(static_cast<uint32_t>(n)));
        off += n;
    }
}

void JrTrainApp::on_start(Core& core) {
    auto& rx = core.rx();
    band_pool_ = std::make_unique<BlockPool>(4096 * sizeof(cf32), 64);
    // jr_train.band の rate は Receiver の work_rate と同じ規則(録音再生では録音の rate から決まる)
    const double in_rate = core.source().config().sample_rate > 0 ? core.source().config().sample_rate : kRfRate;
    const double band_rate = in_rate / std::max(1.0, std::floor(in_rate / ReceiverConfig{}.work_rate_target));
    band_ = std::make_unique<Stream<cf32>>(StreamMeta{"jr_train.band", DataType::ComplexFloat32, band_rate, preset_->center_hz, band_rate, "FS", Direction::RX}, &core.events());
    band_seq_ = 0;
    band_gen_ = 0;

    vp_ = std::make_unique<appfw::ViewProcessor>(rx, info().id + ".view");
    view_.setProcessor(vp_.get());
    vp_->start();
    band_vp_ = std::make_unique<appfw::ViewProcessor>(*band_, info().id + ".band_view", 1024, 512);
    band_view_.setProcessor(band_vp_.get());
    band_vp_->start();

    {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.assign(preset_->channels.size(), {});
        constellation_.clear();
        reached_index_ = 0;
    }
    discontinuities_ = 0;
    dsp_load_ = 0;
    sub_ = rx.subscribe(info().id, DeliveryPolicy::Lossless, 64);
    stop_ = false;
    th_ = std::thread([this] { run(); });
    timer_ = startTimer(200);
}

void JrTrainApp::on_stop() {
    stopRecording();
    if (timer_) {
        killTimer(timer_);
        timer_ = 0;
    }
    stop_ = true;
    if (th_.joinable()) th_.join();
    sub_.reset();
    band_view_.setProcessor(nullptr);
    view_.setProcessor(nullptr);
    band_vp_.reset();
    vp_.reset();
    if (band_) band_->end();
    band_.reset();
    band_pool_.reset();
}

void JrTrainApp::timerEvent(QTimerEvent*) {
    Q_EMIT metersChanged();
    if (recording_) Q_EMIT recordingChanged();
}

void JrTrainApp::run() {
    std::unique_ptr<Receiver> rx;
    std::vector<cf32> iq;
    std::vector<std::complex<float>> points;
    ContinuityChecker cont;
    uint64_t generation = 0;
    bool have_gen = false;
    double busy_ns = 0, span_ns = 0;
    auto last_signal = std::chrono::steady_clock::now();
    while (!stop_) {
        auto d = sub_->pop(100ms);
        if (!d) continue;
        const auto& h = d->block.header();
        if (h.sample_count == 0) continue;
        if (!rx || (core() && core()->source().config().center_freq != built_center_)) {
            if (!core()) continue;
            rx = build_receiver(*core());
            have_gen = false;
        }
        // 連続性: generation の切替(USB 再接続など)と欠落はチャネルの DSP と同期を作り直す(時刻の違う IQ をつながない)
        const auto gap = cont.check(h);
        if ((have_gen && h.generation != generation) || gap) {
            rx->discontinuity();
            ++discontinuities_;
            ++band_gen_;
        }
        generation = h.generation;
        have_gen = true;
        const auto t0 = std::chrono::steady_clock::now();
        auto in = d->block.as<sc16>();
        iq.resize(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) iq[i] = cf32(in[i].real() / 32768.f, in[i].imag() / 32768.f);
        rx->process(iq, h.sample_index);
        // 表示用の値(約 200 ms ごとに EVM とコンスタレーション)
        const bool refresh = t0 - last_signal > 200ms;
        if (refresh) {
            last_signal = t0;
            rx->update_signal();
            const int sel = selected_;
            if (sel < rx->num_channels()) rx->read_signal(sel, points, kConstellationPoints);
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (int c = 0; c < rx->num_channels() && c < static_cast<int>(stats_.size()); ++c) stats_[static_cast<std::size_t>(c)].st = rx->status(c);
            reached_index_ = h.sample_end();
            if (refresh) {
                constellation_.resize(points.size() * 2);
                for (std::size_t i = 0; i < points.size(); ++i) {
                    constellation_[2 * i] = points[i].real();
                    constellation_[2 * i + 1] = points[i].imag();
                }
            }
        }
        const double dt = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        busy_ns = 0.9 * busy_ns + 0.1 * dt;
        span_ns = 0.9 * span_ns + 0.1 * (static_cast<double>(h.sample_count) / in_rate_ * 1e9);
        dsp_load_ = span_ns > 0 ? busy_ns / span_ns : 0;
    }
}

// ---- 録音(radio.rx をそのまま SigMF に。RecorderApp と同じ経路)----

double JrTrainApp::recordSeconds() const {
    if (!rec_ || !core()) return 0;
    const double rate = core()->source().config().sample_rate;
    return rate > 0 ? static_cast<double>(rec_->samples_written()) / rate : 0;
}

void JrTrainApp::toggleRecording() {
    if (recording_) stopRecording();
    else startRecording();
}

void JrTrainApp::startRecording() {
    if (recording_ || !core()) return;
    std::error_code ec;
    std::filesystem::create_directories(record_dir_, ec);
    record_path_ = record_dir_ + "/jr_train_" + preset_->id + "_" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss").toStdString();
    auto& rx = core()->rx();
    rec_ = std::make_unique<SigmfRecorder>(record_path_, rx.meta(), core()->source().config(), &core()->events(), core()->source().lo_correction_ppm());
    if (!rec_->ok()) {
        record_error_ = "cannot open " + QString::fromStdString(record_path_);
        rec_.reset();
        Q_EMIT recordingChanged();
        return;
    }
    record_error_.clear();
    rec_sub_ = rx.subscribe(info().id + ".recorder", DeliveryPolicy::Lossless, 256);
    rec_stop_ = false;
    rec_th_ = std::thread([this] {
        while (!rec_stop_)
            if (auto d = rec_sub_->pop(100ms)) rec_->write(*d);
    });
    recording_ = true;
    core()->events().emit(EventKind::Info, info().id, "recording " + record_path_);
    Q_EMIT recordingChanged();
}

void JrTrainApp::stopRecording() {
    if (!recording_) return;
    rec_stop_ = true;
    if (rec_th_.joinable()) rec_th_.join();
    if (rec_) rec_->close();
    if (rec_sub_ && rec_sub_->stats().dropped_blocks > 0) record_error_ = QString("dropped %1 blocks").arg(rec_sub_->stats().dropped_blocks);
    rec_sub_.reset();
    recording_ = false;
    Q_EMIT recordingChanged();   // rec_ は残す(最後の長さを表示し続ける)
}

} // namespace spear::apps
