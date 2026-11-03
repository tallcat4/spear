#include "std_t99_app.hpp"
#include "spear/core/tap.hpp"

#include <QDateTime>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace spear::apps {

using namespace std::chrono_literals;
using namespace spear::std_t99;

namespace {
constexpr double kRfRate = 4e6;   // 4 Msps → Front ÷10 → 400 kHz → ÷4 → 100 kHz(STD-T98 / SLPR と同じ RF レート。実機で drop 0 の実績)

QString message_text(int type) { return type < 0 ? QString() : QString::fromLatin1(message_name(type)); }

QString utc_text(double utc, const char* format) {
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(utc * 1000)).toString(QString::fromLatin1(format));
}

QVariantMap call_map(const CallInfo& c) {
    QVariantMap m;
    m["number"] = static_cast<double>(c.number);
    m["time"] = utc_text(c.started_utc, "HH:mm:ss");
    m["caller"] = QString::fromStdString(c.caller.value_or(""));
    m["called"] = QString::fromStdString(c.called.value_or(""));
    m["destination"] = QString::fromStdString(c.destination());
    m["callType"] = QString::fromStdString(c.call_type());
    m["name"] = QString::fromStdString(c.name);
    m["nameFailed"] = c.name_failed;
    m["callControl"] = c.call_control;
    m["emergency"] = c.emergency();
    m["kind"] = c.kind >= 0 ? message_text(c.kind) : (c.voice ? QStringLiteral("Voice") : QString());
    m["voice"] = c.voice;
    m["ended"] = c.ended;
    m["frames"] = static_cast<double>(c.frames);
    m["extension"] = c.extension ? QString::asprintf("%05X", *c.extension) : QString();
    QStringList req;
    for (int t : c.requests) req << message_text(t);
    m["requests"] = req.join(", ");
    m["hasPosition"] = c.position.has_value();
    if (c.position) {
        const auto& p = *c.position;
        m["lat"] = p.latitude_deg();
        m["lon"] = p.longitude_deg();
        m["latText"] = QString::asprintf("N %u°%02u.%04u'", p.latitude / 1000000, p.latitude / 10000 % 100, p.latitude % 10000);
        m["lonText"] = QString::asprintf("E %u°%02u.%04u'", p.longitude / 1000000, p.longitude / 10000 % 100, p.longitude % 10000);
        m["altitude"] = p.altitude_m;
        m["gpsValid"] = p.valid;
    }
    m["hasGpsTime"] = c.gps_time.has_value();
    if (c.gps_time) {
        const auto& g = *c.gps_time;
        m["gpsTime"] = g.date == 0 ? QStringLiteral("NO FIX")
                                   : QString::asprintf("20%02u-%02u-%02u %02u:%02u:%02u JST", g.date % 100, g.date / 100 % 100, g.date / 10000, g.time / 10000, g.time / 100 % 100, g.time % 100);
        m["speedKn"] = g.speed_01kn / 10.0;
        m["direction"] = g.direction_deg;
    }
    return m;
}
} // namespace

StdT99App::StdT99App(appfw::AppInfo info, QObject* parent) : appfw::GuiApp(std::move(info), parent) {
    eye_.setCapacity(200);
    eye_.set_rate(2400);
    // 再起動をまたいで残す運転状態(ユーザーが操作する値)。LO オフセットは site.conf
    persist({"selectedChannel", "mute", "standby", "ownMaker", "ownIndividual", "ownGroup",
             "bandView.dbMin", "bandView.dbMax", "bandView.manualRange", "bandView.averaging"});
}

StdT99App::~StdT99App() = default;

void StdT99App::configure(const QVariantMap& settings) {
    if (settings.contains("audio_device")) audio_device_ = settings["audio_device"].toString().toStdString();
    if (settings.contains("record_dir")) record_dir_ = settings["record_dir"].toString().toStdString();
    level_cfg_ = appfw::speech_level(settings, level_cfg_);
    if (settings.contains("std_t99.lo_offset_hz")) lo_offset_hz_ = std::clamp(settings["std_t99.lo_offset_hz"].toDouble(), -1.5e6, 1.5e6);
    if (settings.contains("std_t99.channel")) setSelectedChannel(settings["std_t99.channel"].toInt());
}

RfConfig StdT99App::declare_rf_config() const {
    RfConfig c = GuiApp::declare_rf_config();
    c.sample_rate = kRfRate;
    c.bandwidth = kRfRate;
    c.center_freq = channel_freq_hz(selected_) - lo_offset_hz_;   // LO をチャネルから離す。チャネルは Front の回転で DC に戻す
    return c;
}

QString StdT99App::ownId() const { return QString::fromStdString(normalize_device_id(own_maker_, own_individual_).value_or("")); }

void StdT99App::setSelectedChannel(int ch) {
    ch = std::clamp(ch, 1, kNumChannels);
    if (ch == selected_) return;
    selected_ = ch;
    rebuild_ = true;
    // 運転中は LO を動かす(CH1–9 と CH10–18 は 4 MHz 離れているので 1 つの LO では受けられない)。停止中は草案だけ
    if (core()) core()->source().retune(channel_freq_hz(ch) - lo_offset_hz_);
    else set_rf_config(declare_rf_config());
    Q_EMIT configChanged();
}

void StdT99App::setMute(bool m) { mute_ = m; if (audio_) audio_->set_mute(m); Q_EMIT configChanged(); }

QVariantMap StdT99App::call() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!call_) return {{"present", false}};
    QVariantMap m = call_map(*call_);
    m["present"] = true;
    const double age = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - call_->last_utc;
    m["active"] = !call_->ended && age < 1.0;
    m["accepted"] = accepted_ ? (*accepted_ ? 1 : 0) : -1;
    return m;
}

QVariantList StdT99App::history() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantList out;
    for (const auto& c : history_) out.push_back(call_map(c));
    return out;
}

QVariantMap StdT99App::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantMap m;
    m["locked"] = status_.locked;
    m["frames"] = static_cast<double>(status_.frames);
    m["crcFrames"] = static_cast<double>(status_.crc_frames);
    m["powerDb"] = status_.power_db;
    m["offsetHz"] = status_.freq_offset_hz;
    m["haveOffset"] = status_.have_offset;
    return m;
}

QVariantList StdT99App::pins() const {
    // 最後の送信の位置を赤いピン、履歴にあるほかの発信者の最後の位置を橙の点(std-t99-receiver-sharp と同じ)。
    // 測位無効(V)と日本の範囲の外(緯度 20–46°、経度 122–154°)は立てない
    auto usable = [](const CallInfo& c) {
        if (!c.position || !c.position->valid) return false;
        const double la = c.position->latitude_deg(), lo = c.position->longitude_deg();
        return la >= 20 && la <= 46 && lo >= 122 && lo <= 154;
    };
    std::lock_guard<std::mutex> lk(mu_);
    QVariantList out;
    std::vector<std::string> seen;
    for (std::size_t i = 0; i < history_.size(); ++i) {
        const auto& c = history_[i];
        const std::string who = c.caller.value_or("?");
        if (std::find(seen.begin(), seen.end(), who) != seen.end()) continue;
        seen.push_back(who);
        if (!usable(c)) continue;
        QVariantMap m;
        m["lat"] = c.position->latitude_deg();
        m["lon"] = c.position->longitude_deg();
        m["label"] = QString::fromStdString(c.name.empty() ? who : c.name);
        m["current"] = i == 0;
        out.push_back(m);
    }
    return out;
}

std::unique_ptr<Monitor> StdT99App::build_monitor(Core& core) {
    // 回転量 = 選択チャネル − 実際の LO。実際の LO は Core(単一の所有者)から読む: B210 ならチャネル − loOffset、
    // 録音の再生なら録音時の中心(retune できない)。個体の LO 誤差は Core の Radio が radio.freq_err_ppm で打ち消す
    built_center_ = core.source().config().center_freq;
    const double shift = channel_freq_hz(selected_) - built_center_;
    {
        std::lock_guard<std::mutex> lk(mu_);
        tune_warning_ = std::abs(shift) > 1.6e6 ? QString("CH %1 is outside the received band").arg(selected_.load()) : QString();
        call_.reset();
        history_.clear();
        accepted_.reset();
        status_ = {};
    }
    eye_.clear();
    MonitorObserver o;
    o.band = [this](std::span<const cf32> iq) {
        TAP("std_t99.band", iq.data(), iq.size());
        publish(*band_, *band_pool_, iq, band_seq_, band_index_);
    };
    o.channel = [this](std::span<const cf32> iq) {
        TAP("std_t99.channel", iq.data(), iq.size());
        publish(*channel_, *channel_pool_, iq, chan_seq_, chan_index_);
    };
    o.eye = [this](std::span<const float> t) { eye_.push(t); };
    o.frame = [this](const FrameEvent& e) { on_frame(e); };
    o.voice = [this](const VoiceCommand& c) { if (voice_) voice_->submit(c); };
    return std::make_unique<Monitor>(kRfRate, shift, std::move(o));
}

void StdT99App::on_frame(const FrameEvent& e) {
    // DSP thread(Monitor::process の中)
    if (saving_) {
        const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
        saver_.frame(e.frame, e.json, now);
    }
    if (e.new_call && core()) {
        // 送信の開始だけを event にする(フレームごとには出さない: イベントログを溢れさせない)
        core()->events().emit(EventKind::Info, info().id, "CH" + std::to_string(selected_.load()) + " transmission", {cur_generation_, e.rx_index, e.rx_index + 1}, selected_.load());
    }
}

void StdT99App::on_audio(std::span<const float> pcm, bool play) {
    // 復号スレッド。保存は待受によらず全通話、再生は待受の判定(表 Annex 2-4-14)
    if (saving_) saver_.audio(pcm);
    std::vector<float> a(pcm.begin(), pcm.end());
    TAP("std_t99.audio", a.data(), a.size());
    if (audio_stream_) publish(*audio_stream_, *audio_pool_, std::span<const float>(a.data(), a.size()), audio_seq_, audio_index_);
    if (!play) return;
    if (leveler_) leveler_->process(a);   // 観測点(std_t99.audio)と保存には掛けない
    if (audio_) audio_->write(a);
}

template <class T>
void StdT99App::publish(Stream<T>& s, BlockPool& pool, std::span<const T> x, uint64_t& seq, uint64_t& index) {
    std::size_t off = 0;
    const std::size_t cap = pool.slot_bytes() / sizeof(T);
    while (off < x.size()) {
        const std::size_t n = std::min(cap, x.size() - off);
        auto b = pool.acquire<T>(n);
        std::copy_n(x.data() + off, n, b.template data<T>().data());
        auto& h = b.header();
        h.generation = cur_generation_; h.sequence = seq++; h.sample_index = index;
        index += n;
        s.publish(b.commit(static_cast<uint32_t>(n)));
        off += n;
    }
}

void StdT99App::on_start(Core& core) {
    auto& rx = core.rx();
    const double band_rate = 400e3;
    band_pool_ = std::make_unique<BlockPool>(4096 * sizeof(cf32), 64);
    channel_pool_ = std::make_unique<BlockPool>(2048 * sizeof(cf32), 64);
    audio_pool_ = std::make_unique<BlockPool>(2048 * sizeof(float), 64);
    const double ch_hz = channel_freq_hz(selected_);
    band_ = std::make_unique<Stream<cf32>>(StreamMeta{"std_t99.band", DataType::ComplexFloat32, band_rate, ch_hz, band_rate, "FS", Direction::RX}, &core.events());
    channel_ = std::make_unique<Stream<cf32>>(StreamMeta{"std_t99.channel", DataType::ComplexFloat32, kReceiverRate, ch_hz, kReceiverRate, "FS", Direction::RX}, &core.events());
    audio_stream_ = std::make_unique<Stream<float>>(StreamMeta{"std_t99.audio", DataType::Float32, 8000, 0, 4000, "FS", Direction::RX}, &core.events());
    band_seq_ = band_index_ = chan_seq_ = chan_index_ = audio_seq_ = audio_index_ = 0;

    band_vp_ = std::make_unique<appfw::ViewProcessor>(*band_, info().id + ".band_view", 1024, 512);
    band_view_.setProcessor(band_vp_.get());
    band_vp_->start();
    channel_vp_ = std::make_unique<appfw::ViewProcessor>(*channel_, info().id + ".channel_view", 512, 256);
    channel_view_.setProcessor(channel_vp_.get());
    channel_vp_->start();

    audio_ = std::make_unique<AudioSink>(&core.events(), audio_device_, 8000u);
    leveler_ = appfw::make_speech_leveler(level_cfg_, "std_t99.level", 8000.0);
    audio_->set_mute(mute_);
    std::string err;
    if (!audio_->open(&err)) core.events().emit(EventKind::Warning, info().id, "audio disabled: " + err);
    voice_ = std::make_unique<VoiceWorker>([this](std::span<const float> pcm, bool play) { on_audio(pcm, play); });

    sub_ = rx.subscribe(info().id, DeliveryPolicy::Lossless, 64);
    rebuild_ = true;
    stop_ = false;
    th_ = std::thread([this] { run(); });
    timer_ = startTimer(200);
}

void StdT99App::on_stop() {
    if (timer_) { killTimer(timer_); timer_ = 0; }
    stop_ = true;
    if (th_.joinable()) th_.join();
    sub_.reset();
    voice_.reset();   // 復号スレッドを止める(以降 on_audio は呼ばれない)
    stop_saving();
    stop_recording();
    channel_view_.setProcessor(nullptr);
    band_view_.setProcessor(nullptr);
    channel_vp_.reset();
    band_vp_.reset();
    audio_.reset();
    leveler_.reset();
    if (band_) band_->end();
    if (channel_) channel_->end();
    if (audio_stream_) audio_stream_->end();
    band_.reset();
    channel_.reset();
    audio_stream_.reset();
    band_pool_.reset();
    channel_pool_.reset();
    audio_pool_.reset();
}

void StdT99App::timerEvent(QTimerEvent*) { Q_EMIT metersChanged(); }

void StdT99App::run() {
    std::unique_ptr<Monitor> mon;
    std::vector<cf32> iq;
    double busy_ns = 0, span_ns = 0;
    while (!stop_) {
        auto d = sub_->pop(100ms);
        if (!d) continue;
        const auto& h = d->block.header();
        if (h.sample_count == 0) continue;
        if (rebuild_.exchange(false) || !mon || (core() && core()->source().config().center_freq != built_center_)) {
            if (mon) mon->flush_voice();
            if (core()) mon = build_monitor(*core());
        }
        if (!mon) continue;
        const auto t0 = std::chrono::steady_clock::now();
        cur_generation_ = h.generation;
        mon->set_standby(static_cast<Standby>(standby_.load()), normalize_device_id(own_maker_, own_individual_).value_or(""), own_group_);
        auto in = d->block.as<sc16>();
        iq.resize(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) iq[i] = cf32(in[i].real() / 32768.f, in[i].imag() / 32768.f);
        mon->process(iq, h.sample_index);
        {
            std::lock_guard<std::mutex> lk(mu_);
            call_ = mon->calls().current();
            history_ = mon->calls().history();
            accepted_ = mon->accepted();
            status_ = mon->status();
        }
        const double dt = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        busy_ns = 0.9 * busy_ns + 0.1 * dt;
        span_ns = 0.9 * span_ns + 0.1 * (static_cast<double>(h.sample_count) / kRfRate * 1e9);
        dsp_load_ = span_ns > 0 ? busy_ns / span_ns : 0;
    }
    if (mon) mon->flush_voice();
}

// ---- SAVE(復号結果: .burst / .llr / .jsonl / .wav。原本のプラグインの「録音」)----

void StdT99App::toggleSaving() {
    if (saving_) { stop_saving(); return; }
    if (!core()) return;
    try {
        const auto stem = saver_.start(std::filesystem::path(record_dir_) / "std_t99", selected_);
        saving_ = true;
        core()->events().emit(EventKind::Info, info().id, "saving decoded output " + stem);
    } catch (const std::exception& e) {
        core()->events().emit(EventKind::Warning, info().id, std::string("save failed: ") + e.what());
    }
    Q_EMIT savingChanged();
}

void StdT99App::stop_saving() {
    if (!saving_) return;
    saving_ = false;
    saver_.stop();
    Q_EMIT savingChanged();
}

// ---- REC IQ(radio.rx をそのまま SigMF に。golden 用。JR TRAIN RADIO と同じ経路)----

double StdT99App::recordSeconds() const {
    if (!rec_ || !core()) return 0;
    const double rate = core()->source().config().sample_rate;
    return rate > 0 ? static_cast<double>(rec_->samples_written()) / rate : 0;
}

void StdT99App::toggleRecording() {
    if (recording_) { stop_recording(); return; }
    if (!core()) return;
    std::error_code ec;
    std::filesystem::create_directories(record_dir_, ec);
    record_path_ = record_dir_ + "/std_t99_ch" + std::to_string(selected_.load()) + "_" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss").toStdString();
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

void StdT99App::stop_recording() {
    if (!recording_) return;
    rec_stop_ = true;
    if (rec_th_.joinable()) rec_th_.join();
    if (rec_) rec_->close();
    if (rec_sub_ && rec_sub_->stats().dropped_blocks > 0) record_error_ = QString("dropped %1 blocks").arg(rec_sub_->stats().dropped_blocks);
    rec_sub_.reset();
    recording_ = false;
    Q_EMIT recordingChanged();
}

} // namespace spear::apps
