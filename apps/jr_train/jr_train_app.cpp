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
constexpr unsigned kAudioRate = 8000;       // 合成音声の rate。ALSA / PipeWire 側でリサンプルさせる
constexpr std::size_t kAudioQueue = 125;    // チャネルごとの音声キュー(5 s)。原本と同じ
constexpr uint64_t kFreshUs = 600'000;      // 再生するチャネルに切り替えたとき、これより古い音声は捨てる(原本と同じ)
constexpr std::size_t kPrimeSamples = kAudioRate * 2 / 10;   // 再生の開始時に 200 ms の無音を前置して到着の揺らぎを吸収する
constexpr std::size_t kMessagesShown = 64;

QString service_label(Service s) { return QString::fromLatin1(to_string(s)).toUpper(); }

QString field_text(const protocol::Message& m) {
    QStringList parts;
    for (const auto& f : m.fields) {
        QString v = QString::fromStdString(f.value);
        if (f.english && !f.english->empty()) v += " / " + QString::fromStdString(*f.english);
        parts << QString::fromStdString(f.field) + "=" + v;
    }
    return parts.join("  ");
}
QVariant opt(const std::optional<uint8_t>& v) { return v ? QVariant(static_cast<int>(*v)) : QVariant(); }
} // namespace

JrTrainApp::JrTrainApp(appfw::AppInfo info, QObject* parent) : appfw::GuiApp(std::move(info), parent) {
    catalog_ = presets();
    preset_ = &catalog_.front();
    for (const auto& p : catalog_)
        if (p.id == "east") preset_ = &p;
    recompute_active();
    // 再起動をまたいで残す運転状態(プリセット・LO オフセット・保存内容は site.conf、観測値は宣言しない)
    persist({"selectedChannel", "autoSelect", "mute", "lowerView", "routeSelection", "includeCommon", "bandView.dbMin", "bandView.dbMax", "bandView.manualRange",
             "bandView.averaging"});
}

JrTrainApp::~JrTrainApp() {
    stopRecording();
    saver_.stop();
}

void JrTrainApp::configure(const QVariantMap& settings) {
    // 外部プリセットファイル(原本と同じ形式)。読めなければ組み込みのまま、エラーを画面に出す
    if (settings.contains("jr_train.presets_file") && !settings["jr_train.presets_file"].toString().isEmpty()) {
        try {
            catalog_ = load_presets_file(settings["jr_train.presets_file"].toString().toStdString());
            preset_error_.clear();
        } catch (const std::exception& e) {
            catalog_ = presets();
            preset_error_ = QString::fromStdString(e.what());
        }
        preset_ = &catalog_.front();
    }
    const std::string want = settings.contains("jr_train.preset") ? settings["jr_train.preset"].toString().toStdString() : preset_->id;
    for (const auto& p : catalog_)
        if (p.id == want) preset_ = &p;
    if (preset_->id != want && want != "east") preset_error_ += QString("unknown preset %1; using %2").arg(QString::fromStdString(want), QString::fromStdString(preset_->id));
    if (settings.contains("jr_train.lo_offset_hz")) lo_offset_hz_ = std::clamp(settings["jr_train.lo_offset_hz"].toDouble(), -800e3, 800e3);
    if (settings.contains("jr_train.threads")) threads_ = settings["jr_train.threads"].toInt();
    if (settings.contains("jr_train.save_audio")) save_options_.audio = settings["jr_train.save_audio"].toInt() != 0;
    if (settings.contains("jr_train.save_raw")) save_options_.raw_frames = settings["jr_train.save_raw"].toInt() != 0;
    if (settings.contains("jr_train.save_messages")) save_options_.data_messages = settings["jr_train.save_messages"].toInt() != 0;
    if (settings.contains("record_dir")) record_dir_ = settings["record_dir"].toString().toStdString();
    if (settings.contains("audio_device")) audio_device_ = settings["audio_device"].toString().toStdString();
    recompute_active();
    Q_EMIT configChanged();
}

int JrTrainApp::numChannels() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(active_.size());
}

// 受信するチャネル = 選んだ路線の 3 波(+ 共用波)。選択が空、またはこのプリセットに無い路線だけなら全チャネル
void JrTrainApp::recompute_active() {
    std::vector<std::string> ids;
    for (const auto& id : route_selection_.split(',', Qt::SkipEmptyParts)) ids.push_back(id.trimmed().toStdString());
    auto chans = ids.empty() ? preset_->channels : channels_for_routes(*preset_, ids, include_common_);
    if (chans.empty()) chans = preset_->channels;
    {
        std::lock_guard<std::mutex> lk(mu_);
        active_ = std::move(chans);
        stats_.assign(active_.size(), {});
        constellation_.clear();
        selected_messages_.clear();
        active_calls_.clear();
        selected_ = std::clamp(selected_.load(), 0, static_cast<int>(active_.size()) - 1);
    }
    ++layout_;
    Q_EMIT routeSelectionChanged();
    Q_EMIT selectedChannelChanged();
}

void JrTrainApp::setRouteSelection(const QString& ids) {
    if (ids == route_selection_) return;
    route_selection_ = ids;
    recompute_active();
}

void JrTrainApp::setIncludeCommon(bool on) {
    if (on == include_common_) return;
    include_common_ = on;
    recompute_active();
}

void JrTrainApp::applyRoutes(const QStringList& ids, bool includeCommon) {
    // 受信対象が変わるので、復号結果の保存は区切る(保存はチャネルの組を固定して始める)
    if (saving_) {
        saving_ = false;
        saver_.stop();
        Q_EMIT savingChanged();
    }
    include_common_ = includeCommon;
    route_selection_ = ids.join(',');
    recompute_active();
}

QVariantList JrTrainApp::previewRoutes(const QStringList& ids, bool includeCommon) const {
    std::vector<std::string> v;
    for (const auto& id : ids) v.push_back(id.toStdString());
    QVariantList out;
    for (const auto& c : channels_for_routes(*preset_, v, includeCommon)) {
        QVariantMap m;
        m["label"] = QString::fromStdString(c.short_label);
        m["freq"] = c.frequency_hz;
        out.push_back(m);
    }
    return out;
}

QVariantList JrTrainApp::routes() const {
    QVariantList out;
    for (const auto& r : preset_->routes) {
        QVariantMap m;
        m["id"] = QString::fromStdString(r.id);
        m["line"] = QString::fromStdString(r.line);
        m["section"] = QString::fromStdString(r.section);
        m["designation"] = QString::fromStdString(r.designation);
        m["voice1"] = static_cast<double>(r.voice_hz[0]);
        m["voice2"] = static_cast<double>(r.voice_hz[1]);
        m["data"] = static_cast<double>(r.data_hz);
        m["note"] = QString::fromStdString(r.note);
        out.push_back(m);
    }
    return out;
}

QString JrTrainApp::receiveSet() const {
    const int n = numChannels();
    const auto ids = route_selection_.split(',', Qt::SkipEmptyParts);
    if (ids.isEmpty() || n == static_cast<int>(preset_->channels.size())) return QString("ALL %1 ch").arg(n);
    return QString("ROUTES %1 -> %2 ch").arg(ids.size()).arg(n);
}

RfConfig JrTrainApp::declare_rf_config() const {
    RfConfig c = GuiApp::declare_rf_config();   // 受信端子・利得はシェルの草案のまま
    c.sample_rate = kRfRate;
    c.bandwidth = kRfRate;
    c.center_freq = preset_->center_hz - lo_offset_hz_;
    return c;
}

double JrTrainApp::channelOffsetHz(int ch) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (ch < 0 || ch >= static_cast<int>(active_.size())) return 0;
    return active_[static_cast<std::size_t>(ch)].frequency_hz - preset_->center_hz;
}

std::vector<ChannelConfig> JrTrainApp::channel_configs() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ChannelConfig> out;
    for (const auto& c : active_) out.push_back(channel_config(c));
    return out;
}

void JrTrainApp::setSelectedChannel(int ch) {
    ch = std::clamp(ch, 0, numChannels() - 1);
    if (ch == selected_) return;
    selected_ = ch;
    {
        std::lock_guard<std::mutex> lk(mu_);
        constellation_.clear();
        selected_messages_.clear();
    }
    Q_EMIT selectedChannelChanged();
}

void JrTrainApp::selectChannelByTouch(int ch) {
    setAutoSelect(false);   // 手で選んだチャネルに固定する(原本の UI と同じ)
    setSelectedChannel(ch);
}

void JrTrainApp::setAutoSelect(bool on) {
    if (on == auto_select_) return;
    auto_select_ = on;
    Q_EMIT autoSelectChanged();
}

void JrTrainApp::setMute(bool m) {
    mute_ = m;
    if (audio_) audio_->set_mute(m);
    Q_EMIT muteChanged();
}

void JrTrainApp::setLowerView(const QString& v) {
    if (v == lower_view_ || !(v == "const" || v == "data" || v == "ops")) return;
    lower_view_ = v;
    Q_EMIT lowerViewChanged();
}

QVariantList JrTrainApp::channels() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantList out;
    const auto window = static_cast<uint64_t>(kActiveSeconds * in_rate_);
    const auto active_after = reached_index_ > window ? reached_index_ - window : 0;
    for (std::size_t i = 0; i < active_.size() && i < stats_.size(); ++i) {
        const auto& def = active_[i];
        const auto& s = stats_[i];
        QVariantMap m;
        m["label"] = QString::fromStdString(def.short_label);
        m["name"] = QString::fromStdString(def.label);
        m["freq"] = def.frequency_hz;
        m["offset"] = def.frequency_hz - preset_->center_hz;
        m["service"] = service_label(def.service);
        m["carrier"] = s.st.carrier_locked;
        m["freqErr"] = s.st.frequency_error_hz;
        m["evm"] = s.st.evm_percent;
        m["frames"] = static_cast<double>(s.st.frames);
        m["synced"] = static_cast<double>(s.st.synchronized_frames);
        m["voiceFrames"] = static_cast<double>(s.st.voice_frames);
        m["audioFrames"] = static_cast<double>(s.st.audio_frames);
        m["dataFrames"] = static_cast<double>(s.st.data.valid_frames);
        m["messages"] = static_cast<double>(s.messages);
        m["locked"] = s.st.frame_locked;
        m["active"] = s.ever_synced && s.last_sync_index >= active_after;
        m["call"] = s.call;
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
        m["rejectedVoice"] = static_cast<double>(s.st.rejected_voice);
        m["correctedBits"] = static_cast<double>(s.st.corrected_bits);
        m["inputDbfs"] = s.st.input_dbfs;
        m["dataCorrected"] = static_cast<double>(s.st.data.corrected_frames);
        m["dataRejected"] = static_cast<double>(s.st.data.rejected_frames);
        m["rejectedMessages"] = static_cast<double>(s.st.data.rejected_messages);
        m["resets"] = static_cast<double>(s.resets);
        m["discontinuities"] = static_cast<double>(s.st.discontinuities);
        if (sel < static_cast<int>(active_.size())) m["note"] = QString::fromStdString(active_[static_cast<std::size_t>(sel)].note);
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

QVariantList JrTrainApp::messages() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantList out;
    for (const auto& r : selected_messages_) {
        QVariantMap m;
        m["kind"] = QString::fromStdString(r.message.kind);
        m["identity"] = QString::fromStdString(r.message.identity);
        m["timestamp"] = r.message.timestamp ? QString::fromStdString(*r.message.timestamp) : QString();
        m["fields"] = field_text(r.message);
        m["receptions"] = static_cast<double>(r.receptions);
        out.push_back(m);
    }
    return out;
}

QVariantMap JrTrainApp::operation() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantMap m;
    if (!operation_) return m;
    // 受信日時は PC の時刻(電文の作成日時ではない。前置部は解釈しない: 原本と同じ)
    m["firstReceived"] = QString::fromStdString(operation_->first_received_at);
    m["lastReceived"] = QString::fromStdString(operation_->last_received_at);
    m["receptions"] = static_cast<double>(operation_->receptions);
    QVariantList rows;
    for (const auto& r : operation_->summary.rows) {
        QVariantMap row;
        row["line"] = QString::fromStdString(r.line);
        row["delayed"] = opt(r.delayed);
        row["onLine"] = opt(r.on_line);
        row["class1"] = opt(r.delay_classes[0]);
        row["class2"] = opt(r.delay_classes[1]);
        row["class3"] = opt(r.delay_classes[2]);
        row["maxDelay"] = opt(r.max_delay_min);
        row["trainNo"] = r.train_no ? QString::fromStdString(*r.train_no) : QString();
        rows.push_back(row);
    }
    m["rows"] = rows;
    return m;
}

QVariantList JrTrainApp::activeCalls() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantList out;
    for (int c : active_calls_) out.push_back(c);
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
    c.band_center_hz = preset_->center_hz;
    c.threads = threads_;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rx_channels_ = active_;
        built_layout_ = layout_;
        stats_.assign(active_.size(), {});
    }
    for (const auto& ch : rx_channels_) c.channels.push_back(channel_config(ch));
    auto rx = std::make_unique<Receiver>(c);
    in_rate_ = c.in_rate;
    dsp_threads_ = rx->threads();
    chan_audio_.assign(c.channels.size(), {});
    std::vector<std::string> ids;
    for (const auto& ch : c.channels) ids.push_back(ch.id);
    monitor_ = std::make_unique<CallMonitor>(ids);
    Observer o;
    o.band = [this](std::span<const cf32> iq, uint64_t first) {
        TAP("jr_train.band", iq.data(), iq.size());
        publish_band(iq, first);
    };
    o.frame = [this, &core, configs = c.channels](int ch, const FrameEvent& ev, const FrameTiming& t) {
        const auto c_index = static_cast<std::size_t>(ch);
        if (saving_) saver_.push_frame(configs[c_index].id, ev);
        // 通話の推定(原本 session: 区間の切り替わりは Reset、受理した音声フレームは Voice)
        auto& ca = chan_audio_[c_index];
        const uint64_t back = static_cast<uint64_t>(ev.index) * 40'000;
        ca.audio_origin_us = t.time_us > back ? t.time_us - back : 0;
        if (ca.have_epoch && ev.epoch != ca.frame_epoch) ca.activity.push_back({CallMonitor::Activity::Reset, t.time_us});
        ca.frame_epoch = ev.epoch;
        ca.have_epoch = true;
        if (ev.corrected_bits) ca.activity.push_back({CallMonitor::Activity::Voice, t.time_us});
        bool first_sync = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (built_layout_ != layout_ || c_index >= stats_.size()) return;   // 受信対象が変わった(次のブロックで組み直す)
            auto& s = stats_[c_index];
            s.messages += ev.messages.size();
            if (ev.frame.sync_matched) {
                first_sync = !s.ever_synced;
                s.last_sync_index = t.input_sample_index;
                s.ever_synced = true;
            }
        }
        // 全フレームを event にすると多すぎる(22 波 × 25 /s)。チャネルごとの最初の同期だけ記録する
        if (first_sync) {
            const auto& def = rx_channels_[c_index];
            const uint64_t span = static_cast<uint64_t>(protocol::kFrameBits / 2 * in_rate_ / jr_train::dsp::kSymbolRate);
            core.events().emit(EventKind::Info, info().id, "frame sync " + def.short_label + " " + std::to_string(def.frequency_hz / 1e6).substr(0, 9) + " MHz",
                               {0, t.input_sample_index, t.input_sample_index + span}, ch);
        }
    };
    o.audio = [this](int ch, const Audio& a) {
        if (saving_) saver_.push(a);
        auto& ca = chan_audio_[static_cast<std::size_t>(ch)];
        if (a.epoch != ca.epoch) {
            ca.queue.clear();
            ca.epoch = a.epoch;
        }
        if (ca.queue.size() == kAudioQueue) ca.queue.pop_front();
        ca.queue.emplace_back(ca.audio_origin_us + static_cast<uint64_t>(a.index) * 40'000, a);
    };
    rx->set_observer(std::move(o));
    return rx;
}

// 原本 session::snapshot_with_monitor: 通話の監視を進め、再生するチャネルの音声を出す
void JrTrainApp::deliver_audio(Receiver& rx) {
    const uint64_t progress = rx.time_us();
    for (std::size_t c = 0; c < chan_audio_.size(); ++c) {
        monitor_->update(c, progress, chan_audio_[c].activity);
        chan_audio_[c].activity.clear();
    }
    const auto [preferred, calls] = monitor_->snapshot();
    const auto index_of = [&](const std::string& id) {
        for (std::size_t c = 0; c < rx_channels_.size(); ++c)
            if (channel_config(rx_channels_[c]).id == id) return static_cast<int>(c);
        return -1;
    };
    // AUTO: 最優先の通話、無ければ選択中のチャネル(原本 preferred.or(selected))。手動: 選択中のチャネル
    int monitored = selected_;
    if (auto_select_ && preferred) monitored = index_of(*preferred);
    std::vector<int> call_indices;
    for (const auto& id : calls) call_indices.push_back(index_of(id));
    const int previous = monitored_.exchange(monitored);
    if (auto_select_ && preferred && monitored != selected_ && monitored >= 0) {
        selected_ = monitored;   // AUTO 中は表示も再生中のチャネルに追従する
        QMetaObject::invokeMethod(this, [this] { Q_EMIT selectedChannelChanged(); }, Qt::QueuedConnection);
    }
    std::vector<float> out;
    for (std::size_t c = 0; c < chan_audio_.size(); ++c) {
        auto& ca = chan_audio_[c];
        const bool now_monitored = static_cast<int>(c) == monitored;
        // 新しく選んだチャネルでは古い音声(600 ms より前)を捨てる。聴き続けている間は捨てない(原本と同じ)
        if (!now_monitored || !ca.monitored)
            while (!ca.queue.empty() && ca.queue.front().first + kFreshUs < progress) ca.queue.pop_front();
        ca.monitored = now_monitored;
        if (!now_monitored) continue;
        for (const auto& [t, a] : ca.queue)
            for (int16_t s : a.pcm) out.push_back(static_cast<float>(s) / 32768.f);
        ca.queue.clear();
    }
    if (previous != monitored) audio_primed_ = false;
    if (audio_ && !out.empty()) {
        if (!audio_primed_) {
            const std::vector<float> silence(kPrimeSamples, 0.f);
            audio_->write(silence);
            audio_primed_ = true;
        }
        audio_->write(out);
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (built_layout_ != layout_) return;
    active_calls_ = call_indices;
    for (std::size_t c = 0; c < stats_.size(); ++c) stats_[c].call = std::find(call_indices.begin(), call_indices.end(), static_cast<int>(c)) != call_indices.end();
}

void JrTrainApp::refresh_display(Receiver& rx) {
    const int sel = selected_;
    std::vector<std::complex<float>> points;
    std::vector<Received> sel_messages;
    std::optional<protocol::OperationReception> op;
    std::vector<PipelineStatus> statuses;
    for (int c = 0; c < rx.num_channels(); ++c) {
        const auto& st = rx.snapshot(c);
        if (c == sel) {
            points = st.constellation;
            sel_messages.assign(st.messages.begin(), st.messages.begin() + static_cast<std::ptrdiff_t>(std::min(st.messages.size(), kMessagesShown)));
        }
        if (st.operation_monitor && (!op || st.operation_monitor->last_received_at > op->last_received_at)) op = st.operation_monitor;
        PipelineStatus copy = st;
        copy.constellation.clear();
        copy.messages.clear();
        statuses.push_back(std::move(copy));
        rx.take_messages(c);   // 電文は FrameEvent で受け取る(保存・表示)。Pipeline のキューは溜めずに捨てる
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (built_layout_ != layout_) return;
    for (int c = 0; c < rx.num_channels() && c < static_cast<int>(stats_.size()); ++c) {
        stats_[static_cast<std::size_t>(c)].st = std::move(statuses[static_cast<std::size_t>(c)]);
        stats_[static_cast<std::size_t>(c)].resets = rx.resets(c);
    }
    constellation_.resize(std::min(points.size(), kConstellationPoints) * 2);
    for (std::size_t i = 0; i < constellation_.size() / 2; ++i) {
        constellation_[2 * i] = points[i].real();
        constellation_[2 * i + 1] = points[i].imag();
    }
    selected_messages_ = std::move(sel_messages);
    if (op) operation_ = op;
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

    audio_ = std::make_unique<AudioSink>(&core.events(), audio_device_, kAudioRate, kAudioRate * 2);
    audio_->set_mute(mute_);
    std::string err;
    if (!audio_->open(&err)) core.events().emit(EventKind::Warning, info().id, "audio disabled: " + err);
    audio_primed_ = false;

    {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.assign(active_.size(), {});
        constellation_.clear();
        selected_messages_.clear();
        active_calls_.clear();
        operation_.reset();
        reached_index_ = 0;
    }
    monitored_ = -1;
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
    if (saving_) {   // DSP thread を止めてから(最後の音声まで渡し終えてから)保存を確定する
        saving_ = false;
        saver_.stop();
        Q_EMIT savingChanged();
    }
    sub_.reset();
    band_view_.setProcessor(nullptr);
    view_.setProcessor(nullptr);
    band_vp_.reset();
    vp_.reset();
    audio_.reset();
    if (band_) band_->end();
    band_.reset();
    band_pool_.reset();
}

void JrTrainApp::timerEvent(QTimerEvent*) {
    Q_EMIT metersChanged();
    if (recording_) Q_EMIT recordingChanged();
    if (saving_) Q_EMIT savingChanged();
}

void JrTrainApp::run() {
    std::unique_ptr<Receiver> rx;
    std::vector<cf32> iq;
    ContinuityChecker cont;
    uint64_t generation = 0;
    bool have_gen = false;
    double busy_ns = 0, span_ns = 0;
    auto last_display = std::chrono::steady_clock::now();
    while (!stop_) {
        auto d = sub_->pop(100ms);
        if (!d) continue;
        const auto& h = d->block.header();
        if (h.sample_count == 0) continue;
        if (!rx || layout_ != built_layout_ || (core() && core()->source().config().center_freq != built_center_)) {
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
            for (auto& ca : chan_audio_) {
                ca.queue.clear();
                if (ca.have_epoch) ca.activity.push_back({CallMonitor::Activity::Reset, rx->time_us()});
            }
        }
        generation = h.generation;
        have_gen = true;
        const auto t0 = std::chrono::steady_clock::now();
        auto in = d->block.as<sc16>();
        iq.resize(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) iq[i] = cf32(in[i].real() / 32768.f, in[i].imag() / 32768.f);
        rx->process(iq, h.sample_index);
        deliver_audio(*rx);
        if (t0 - last_display > 200ms) {
            last_display = t0;
            refresh_display(*rx);
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            reached_index_ = h.sample_end();
        }
        const double dt = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        busy_ns = 0.9 * busy_ns + 0.1 * dt;
        span_ns = 0.9 * span_ns + 0.1 * (static_cast<double>(h.sample_count) / in_rate_ * 1e9);
        dsp_load_ = span_ns > 0 ? busy_ns / span_ns : 0;
    }
}

// ---- 保存(復号結果。原本と同じ形式)----

bool JrTrainApp::saving() const { return saving_; }

QVariantMap JrTrainApp::saveStatus() const {
    const auto s = saver_.snapshot();
    QVariantMap m;
    m["state"] = QString::fromStdString(s.state);
    m["directory"] = s.directory ? QString::fromStdString(*s.directory) : QString();
    m["files"] = static_cast<double>(s.files);
    m["seconds"] = static_cast<double>(s.pcm_samples) / kAudioRate;
    m["rawFrames"] = static_cast<double>(s.raw_frames);
    m["messages"] = static_cast<double>(s.data_messages);
    m["error"] = s.error ? QString::fromStdString(*s.error) : QString();
    return m;
}

void JrTrainApp::toggleSaving() {
    if (saving_) {
        saving_ = false;
        saver_.stop();
    } else if (core()) {
        std::error_code ec;
        const auto parent = std::filesystem::absolute(std::filesystem::path(record_dir_) / "jr_train", ec);
        std::filesystem::create_directories(parent, ec);
        try {
            saver_.start(parent, channel_configs(), save_options_);
            saving_ = true;
            core()->events().emit(EventKind::Info, info().id, "saving decoded output " + saver_.snapshot().directory.value_or(""));
        } catch (const std::exception& e) {
            core()->events().emit(EventKind::Warning, info().id, std::string("save failed: ") + e.what());
        }
    }
    Q_EMIT savingChanged();
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
