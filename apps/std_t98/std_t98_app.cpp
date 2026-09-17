#include "std_t98_app.hpp"
#include "spear/core/tap.hpp"

#include <QDateTime>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace spear::apps {

using namespace std::chrono_literals;
using namespace spear::std_t98;

namespace {
constexpr double kRfRate = 4e6;        // 4 Msps → resamp1 ÷10 → 400 kHz = 64 × 6.25 kHz(実機録音で検証済みの構成)
constexpr int kPfb = 64;
constexpr unsigned kAudioRate = 8000;  // AMBE の出力レート。ALSA/PipeWire 側でリサンプルさせる
constexpr float kAmbeGain = 7.f / 32768.f;   // mbelib の short 経路(float × 7)相当で ±1.0 に
constexpr double kAudioPerInput = kAudioRate / kRfRate;   // radio.rx index → 8 kHz index
constexpr uint64_t kMixLatency = kAudioRate * 3 / 10;     // 300 ms: フレーム長 80 ms + 受信機の遅延 + ブロック長より十分大きく
constexpr std::size_t kMixRing = 16384;                   // 2 s
}

StdT98App::StdT98App(appfw::AppInfo info, QObject* parent) : appfw::GuiApp(std::move(info), parent) {
    cfg_.in_rate = kRfRate;
    cfg_.pfb_channels = kPfb;
    cfg_.squelch_db = squelch_db_;
    stats_.resize(static_cast<std::size_t>(cfg_.num_channels));
    eye_.setCapacity(200);
    eye_.set_rate(cfg_.baud);
}
StdT98App::~StdT98App() = default;

void StdT98App::configure(const QVariantMap& settings) {
    if (settings.contains("audio_device")) audio_device_ = settings["audio_device"].toString().toStdString();
    if (settings.contains("std_t98.freq_err_hz")) freq_err_hz_ = settings["std_t98.freq_err_hz"].toDouble();
    if (settings.contains("std_t98.band_center_hz")) band_center_hz_ = settings["std_t98.band_center_hz"].toDouble();
    if (settings.contains("std_t98.squelch_db")) squelch_db_ = settings["std_t98.squelch_db"].toDouble();
    if (settings.contains("std_t98.secret")) secret_enabled_ = settings["std_t98.secret"].toInt() != 0;
}

RfConfig StdT98App::declare_rf_config() const {
    RfConfig c = GuiApp::declare_rf_config();
    c.sample_rate = kRfRate;
    c.bandwidth = kRfRate;
    c.center_freq = band_center_hz_ - lo_offset_hz_;   // LO を帯域中心から離す。帯域は Receiver の回転で戻す
    return c;
}

double StdT98App::channelFreqHz(int ch) const { return band_center_hz_ + channelOffsetHz(ch); }

void StdT98App::retune() {
    rebuild_ = true;
    if (core()) core()->source().retune(band_center_hz_ - lo_offset_hz_);
    else { RfConfig c = GuiApp::declare_rf_config(); c.center_freq = band_center_hz_ - lo_offset_hz_; set_rf_config(c); }
    Q_EMIT configChanged();
}
void StdT98App::setBandCenterHz(double hz) { band_center_hz_ = hz; retune(); }
void StdT98App::setLoOffsetHz(double hz) {
    lo_offset_hz_ = std::clamp(hz, -(kRfRate / 2 - 300e3), kRfRate / 2 - 300e3);
    retune();
}

void StdT98App::setSelectedChannel(int ch) {
    ch = std::clamp(ch, 0, cfg_.num_channels - 1);
    if (ch == selected_) return;
    selected_ = ch;
    eye_.clear();
    Q_EMIT configChanged();
}

void StdT98App::setVolume(double v) { volume_ = std::clamp(v, 0.0, 1.0); if (audio_) audio_->set_volume(static_cast<float>(volume_)); Q_EMIT configChanged(); }
void StdT98App::setMute(bool m) { mute_ = m; if (audio_) audio_->set_mute(m); Q_EMIT configChanged(); }

QVariantList StdT98App::channels() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantList out;
    for (int i = 0; i < cfg_.num_channels; ++i) {
        const auto& s = stats_[static_cast<std::size_t>(i)];
        QVariantMap m;
        m["ch"] = i + 1;
        m["freq"] = channelFreqHz(i);
        m["power"] = s.power_db;
        m["open"] = s.open;
        m["frames"] = static_cast<double>(s.frames);
        m["sacchOk"] = static_cast<double>(s.sacch_ok);
        m["pichOk"] = static_cast<double>(s.pich_ok);
        m["syncs"] = static_cast<double>(s.syncs);
        m["bestSse"] = s.best_sse;
        m["csm"] = QString::fromStdString(s.csm);
        m["secret"] = s.secret;
        m["key"] = s.key;
        m["secretStatus"] = QString::fromLatin1(std_t98::secret::to_string(s.secret_status));
        out.push_back(m);
    }
    return out;
}

QVariantMap StdT98App::selected() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto& s = stats_[static_cast<std::size_t>(selected_.load())];
    QVariantMap m;
    m["ch"] = selected_.load() + 1;
    m["freq"] = channelFreqHz(selected_);
    m["power"] = s.power_db;
    m["open"] = s.open;
    m["frames"] = static_cast<double>(s.frames);
    m["sacchOk"] = static_cast<double>(s.sacch_ok);
    m["pichOk"] = static_cast<double>(s.pich_ok);
    m["syncs"] = static_cast<double>(s.syncs);
    m["bestSse"] = s.best_sse;
    m["sps"] = s.sps;
    m["csm"] = QString::fromStdString(s.csm);
    m["freqErrEst"] = s.freq_err_est;
    m["lastFrame"] = last_frame_;
    m["secret"] = s.secret;
    m["key"] = s.key;
    m["secretStatus"] = QString::fromLatin1(std_t98::secret::to_string(s.secret_status));
    m["secretSource"] = QString::fromLatin1(std_t98::secret::to_string(s.last_source));
    m["secretSeconds"] = s.last_search_s;
    m["secretSearches"] = static_cast<double>(s.searches);
    return m;
}

QString StdT98App::secretStatus() const {
    if (!secret_enabled_) return "OFF";
    if (!secret_) return "--";
    const auto st = secret_->status();
    if (!st.ready) return st.error.empty() ? "LOADING" : "ERROR: " + QString::fromStdString(st.error);
    return QString("ready  req %1 done %2").arg(st.requests).arg(st.results);
}

QVariantList StdT98App::secretCache() const {
    QVariantList out;
    std::lock_guard<std::mutex> lk(secret_mu_);
    for (uint16_t k : secret_cache_) out.push_back(k);
    return out;
}

std::unique_ptr<Receiver> StdT98App::build_receiver(Core& core) {
    ReceiverConfig c = cfg_;
    // 帯域中心を DC に戻す回転 = 個体誤差 + (帯域中心 − 実際の LO)。実際の LO は Core(単一の所有者)から読む:
    // B210 なら band − loOffset、録音再生なら録音時の中心(retune できない)なのでオフセットは自然に 0 になる
    built_center_ = core.source().config().center_freq;
    c.freq_err_hz = freq_err_hz_ + (band_center_hz_ - built_center_);
    c.squelch_db = squelch_db_;
    auto rx = std::make_unique<Receiver>(c);
    Observer o;
    o.band = [this](std::span<const cf32> iq, uint64_t) {
        TAP("std_t98.band", iq.data(), iq.size());
        BlockHeader h; h.generation = 0;
        publish(*band_, *band_pool_, iq, band_seq_, band_index_, h);
    };
    o.channel_iq = [this](int ch, std::span<const cf32> iq, uint64_t) {
        if (ch != selected_) return;
        BlockHeader h; h.generation = 0;
        publish(*channel_, *channel_pool_, iq, chan_seq_, chan_index_, h);
    };
    o.eye = [this](int ch, std::span<const float> t) { if (ch == selected_) eye_.push(t); };
    o.discriminator = [this](int ch, std::span<const float> d) {
        // 周波数誤差の推定: 開いているチャネルの discriminator 平均 × 偏移(315 Hz)を IIR で
        double m = 0;
        for (float v : d) m += v;
        m = d.empty() ? 0 : m / static_cast<double>(d.size()) * cfg_.fsk_dev_hz;
        std::lock_guard<std::mutex> lk(mu_);
        auto& s = stats_[static_cast<std::size_t>(ch)];
        if (s.open) s.freq_err_est = 0.95 * s.freq_err_est + 0.05 * m;
    };
    o.frame = [this, &core](const Frame& f) {
        ++total_frames_;
        QVariantMap m;
        m["ch"] = f.channel + 1;
        m["symbolIndex"] = static_cast<double>(f.symbol_index);
        m["inputIndex"] = static_cast<double>(f.input_sample_index);
        m["sse"] = f.sync_sse;
        m["richF"] = f.rich.f; m["richM"] = f.rich.m; m["richOk"] = f.rich.parity_ok;
        QString line = QString("ch%1 sym %2 sse %3 RICH F=%4 M=%5 %6").arg(f.channel + 1, 2, 10, QChar('0')).arg(f.symbol_index).arg(f.sync_sse, 0, 'f', 2).arg(f.rich.f).arg(f.rich.m).arg(f.rich.parity_ok ? "ok" : "BAD");
        if (f.rich.f == 0) {
            m["type"] = "PICH"; m["csm"] = QString::fromStdString(f.pich.csm); m["crcOk"] = f.pich.crc_ok; m["bitErrors"] = f.pich.bit_errors;
            line += QString("  PICH csm=%1 crc=%2 err=%3").arg(QString::fromStdString(f.pich.csm)).arg(f.pich.crc_ok ? "ok" : "BAD").arg(f.pich.bit_errors);
        } else if (f.rich.f == 1) {
            m["type"] = "SACCH"; m["msgType"] = f.sacch.msg_type; m["callStat"] = f.sacch.call_stat; m["userCode"] = f.sacch.user_code; m["makerCode"] = f.sacch.maker_code;
            m["crcOk"] = f.sacch.crc_ok; m["bitErrors"] = f.sacch.bit_errors;
            line += QString("  SACCH msg=%1 call=%2 user=%3 maker=%4 crc=%5 err=%6").arg(f.sacch.msg_type).arg(f.sacch.call_stat).arg(f.sacch.user_code).arg(f.sacch.maker_code).arg(f.sacch.crc_ok ? "ok" : "BAD").arg(f.sacch.bit_errors);
        } else {
            m["type"] = "RICH F=" + QString::number(f.rich.f);
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (f.channel == selected_) last_frame_ = m;
            frame_log_.prepend(line);
            while (frame_log_.size() > 10) frame_log_.removeLast();
        }
        // フレーム event(provenance: radio.rx の index、192 シンボル = 192 × in_rate / baud サンプル)
        const uint64_t span = static_cast<uint64_t>(192.0 * cfg_.in_rate / cfg_.baud);
        core.events().emit(EventKind::Info, info().id, "frame " + line.toStdString(), {0, f.input_sample_index, f.input_sample_index + span}, static_cast<int64_t>(f.channel + 1));
        // 同期バースト = 呼の区切り: 秘話セッションを閉じる(鍵は保持)
        if (f.rich.f == 0) secret_clear(f.channel);
        // トラフィック: 秘話判定 → 音声(AMBE → 8 kHz PCM → ミキサ)→ 秘話なら鍵探索の窓へ
        if (f.rich.f == 1 && f.tch_payload.size() == 36) traffic_frame(f);
    };
    rx->set_observer(std::move(o));
    return rx;
}

void StdT98App::secret_clear(int ch) {
    auto& sc = secret_ch_[static_cast<std::size_t>(ch)];
    sc.call_secret = false;
    sc.tracker.on_clear();
}

// worker thread → DSP thread: 結果をトラッカーに渡し、鍵が変われば PN を作り直す
void StdT98App::secret_drain_results() {
    std::vector<secret::Worker::Result> results;
    { std::lock_guard<std::mutex> lk(secret_mu_); results.swap(secret_results_); }
    for (const auto& r : results) {
        auto& sc = secret_ch_[static_cast<std::size_t>(r.channel)];
        sc.tracker.on_result(r.session, r.key);
        std::lock_guard<std::mutex> lk(mu_);
        auto& s = stats_[static_cast<std::size_t>(r.channel)];
        s.last_source = r.source; s.last_search_s = r.seconds; ++s.searches;
    }
}

void StdT98App::traffic_frame(const Frame& f) {
    auto& sc = secret_ch_[static_cast<std::size_t>(f.channel)];
    // 秘話判定は CRC OK の SACCH だけで更新する(tools は CRC 不良を平文扱いにしてセッションを捨てていたが、SACCH は 2% 程度落ちる)
    if (f.sacch.crc_ok) sc.call_secret = (f.sacch.call_stat == 1);
    if (!sc.call_secret) sc.tracker.on_clear();
    const bool keyed = sc.call_secret && sc.tracker.key() != 0;
    if (keyed && sc.pn_key != sc.tracker.key()) { sc.pn_key = sc.tracker.key(); sc.pn = secret::pn_sequence(sc.pn_key); }

    // 音声(全チャネル or 選択チャネル)。秘話で鍵が無い間は tools と同じくスクランブルのまま鳴る(活動が分かる)
    const bool want_audio = audio_ && (all_audio_ || f.channel == selected_);
    secret::Burst raw{};
    bool have_raw = false;
    if (want_audio) {
        auto& dec = decoders_[static_cast<std::size_t>(f.channel)];
        if (!dec) dec = std::make_unique<AmbeDecoder>();
        const uint64_t pos0 = static_cast<uint64_t>(static_cast<double>(f.input_sample_index) * kAudioPerInput);
        const bool late = mix_.started && pos0 < mix_.play_pos;                       // 再生済みの時刻(遅延 300 ms より遅れて復号された)
        const bool future = mix_.started && pos0 + 640 > mix_.play_pos + kMixRing;   // 未来すぎる(generation 切替直後など)
        if (late) ++audio_late_;
        for (int k = 0; k < 4; ++k) {
            std::array<int16_t, 160> s{};
            const auto d = dec->decode_3600(std::span<const uint8_t, 9>(f.tch_payload.data() + k * 9, 9), s, keyed ? secret::keystream_d(sc.pn, k) : nullptr);
            raw[static_cast<std::size_t>(k)] = secret::unpack_frame49(d.raw2450);
            if (late || future) continue;
            for (int i = 0; i < 160; ++i) {
                const uint64_t pos = pos0 + static_cast<uint64_t>(k * 160 + i);
                mix_.ring[pos % kMixRing] += static_cast<float>(s[static_cast<std::size_t>(i)]) * kAmbeGain;
            }
        }
        have_raw = true;
    }
    if (!sc.call_secret || !secret_) return;
    // 鍵探索の窓: FEC 後・XOR 前の 49 bit × 4。音声を出していないチャネルでも積む(選択を切り替えた瞬間から復号できるように)
    if (!have_raw)
        for (int k = 0; k < 4; ++k) raw[static_cast<std::size_t>(k)] = secret::unpack_frame49(fec_demod_3600_to_2450(std::span<const uint8_t, 9>(f.tch_payload.data() + k * 9, 9)));
    secret::Tracker::Request rq;
    if (sc.tracker.on_secret_burst(raw, &rq)) secret_->submit({f.channel, rq.session, rq.current_key, std::move(rq.bursts)});
}

void StdT98App::mix_flush(uint64_t reached_input_index) {
    const uint64_t reached = static_cast<uint64_t>(static_cast<double>(reached_input_index) * kAudioPerInput);
    if (!mix_.started || reached < mix_.last_reached) {   // 開始 / generation 切替: 遅延分だけ後ろから
        std::fill(mix_.ring.begin(), mix_.ring.end(), 0.f);
        mix_.play_pos = reached > kMixLatency ? reached - kMixLatency : 0;
        mix_.started = true;
    }
    mix_.last_reached = reached;
    if (reached < kMixLatency) return;
    const uint64_t target = reached - kMixLatency;
    if (target <= mix_.play_pos) return;
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(target - mix_.play_pos));
    for (uint64_t p = mix_.play_pos; p < target; ++p) {
        float& v = mix_.ring[p % kMixRing];
        out.push_back(std::clamp(v, -1.f, 1.f));
        v = 0.f;
    }
    mix_.play_pos = target;
    if (audio_) audio_->write(out);
}

void StdT98App::publish(Stream<cf32>& s, BlockPool& pool, std::span<const cf32> x, uint64_t& seq, uint64_t& index, const BlockHeader& src) {
    std::size_t off = 0;
    const std::size_t cap = pool.slot_bytes() / sizeof(cf32);
    while (off < x.size()) {
        const std::size_t n = std::min(cap, x.size() - off);
        auto b = pool.acquire<cf32>(n);
        std::copy_n(x.data() + off, n, b.data<cf32>().data());
        auto& h = b.header();
        h.generation = src.generation; h.sequence = seq++; h.sample_index = index; h.host_ns = src.host_ns;
        index += n;
        s.publish(b.commit(static_cast<uint32_t>(n)));
        off += n;
    }
}

void StdT98App::on_start(Core& core) {
    auto& rx = core.rx();
    const double band_rate = kPfb * cfg_.spacing_hz;
    band_pool_ = std::make_unique<BlockPool>(4096 * sizeof(cf32), 64);
    channel_pool_ = std::make_unique<BlockPool>(2048 * sizeof(cf32), 64);
    band_ = std::make_unique<Stream<cf32>>(StreamMeta{"std_t98.band", DataType::ComplexFloat32, band_rate, band_center_hz_, band_rate, "FS", Direction::RX}, &core.events());
    channel_ = std::make_unique<Stream<cf32>>(StreamMeta{"std_t98.channel", DataType::ComplexFloat32, cfg_.spacing_hz * cfg_.resamp2, channelFreqHz(selected_), cfg_.spacing_hz, "FS", Direction::RX}, &core.events());
    band_seq_ = band_index_ = chan_seq_ = chan_index_ = 0;

    vp_ = std::make_unique<appfw::ViewProcessor>(rx, info().id + ".view");
    view_.setProcessor(vp_.get());
    vp_->start();
    band_vp_ = std::make_unique<appfw::ViewProcessor>(*band_, info().id + ".band_view", 1024, 512);
    band_view_.setProcessor(band_vp_.get());
    band_vp_->start();
    channel_vp_ = std::make_unique<appfw::ViewProcessor>(*channel_, info().id + ".channel_view", 256, 256);
    channel_view_.setProcessor(channel_vp_.get());
    channel_vp_->start();

    audio_ = std::make_unique<AudioSink>(&core.events(), audio_device_, kAudioRate);
    audio_->set_volume(static_cast<float>(volume_));
    audio_->set_mute(mute_);
    std::string err;
    if (!audio_->open(&err)) core.events().emit(EventKind::Warning, info().id, "audio disabled: " + err);

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : stats_) s = ChanStat{};
        frame_log_.clear();
        last_frame_.clear();
    }
    total_frames_ = 0;
    audio_late_ = 0;
    mix_ = Mixer{};
    mix_.ring.assign(kMixRing, 0.f);
    decoders_.clear();
    decoders_.resize(static_cast<std::size_t>(cfg_.num_channels));
    // 秘話: 鍵探索ワーカー(モデルのロードもワーカースレッド上)。結果は DSP thread が次のブロックで取り込む
    secret_ch_.assign(static_cast<std::size_t>(cfg_.num_channels), SecretChan{});
    { std::lock_guard<std::mutex> lk(secret_mu_); secret_results_.clear(); secret_cache_.clear(); }
    if (secret_enabled_) {
        EventBus* events = &core.events();
        const std::string id = info().id;
        secret_ = std::make_unique<secret::Worker>(
            [this, events, id](const secret::Worker::Result& r) {
                { std::lock_guard<std::mutex> lk(secret_mu_); secret_results_.push_back(r); secret_cache_ = r.cache; }
                events->emit(r.key ? EventKind::Info : EventKind::Warning, id,
                             "secret ch" + std::to_string(r.channel + 1) + " key " + std::to_string(r.key) + " (" + secret::to_string(r.source) + ", " + std::to_string(r.seconds).substr(0, 4) + " s)",
                             {}, r.key);
            },
            [events, id](const std::string& msg) { events->emit(msg.rfind("secret disabled", 0) == 0 ? EventKind::Warning : EventKind::Info, id, msg); });
    }
    sub_ = rx.subscribe(info().id, DeliveryPolicy::Lossless, 64);
    rebuild_ = true;
    stop_ = false;
    th_ = std::thread([this] { run(); });
    timer_ = startTimer(200);
}

void StdT98App::on_stop() {
    if (timer_) { killTimer(timer_); timer_ = 0; }
    stop_ = true;
    if (th_.joinable()) th_.join();
    secret_.reset();   // 探索中なら打ち切って join
    sub_.reset();
    channel_view_.setProcessor(nullptr);
    band_view_.setProcessor(nullptr);
    view_.setProcessor(nullptr);
    channel_vp_.reset();
    band_vp_.reset();
    vp_.reset();
    audio_.reset();
    if (band_) band_->end();
    if (channel_) channel_->end();
    band_.reset();
    channel_.reset();
    band_pool_.reset();
    channel_pool_.reset();
    eye_.clear();
}

void StdT98App::timerEvent(QTimerEvent*) {
    // チャネル IQ 表示は帯域表示と同じ dBFS スケール(PFB・補間とも利得 1)なので、レンジを帯域表示に追従させる。
    // 帯域側のレンジ(自動決定 / REF キー)が単一の所有者で、チャネル側はその写しを持たない。
    if (band_vp_ && channel_vp_) {
        float lo, hi, clo, chi;
        band_vp_->get_db_range(lo, hi);
        channel_vp_->get_db_range(clo, chi);
        if (lo != clo || hi != chi) { channel_vp_->set_db_range(lo, hi); Q_EMIT channel_view_.rangeChanged(); }
    }
    Q_EMIT metersChanged();
}

void StdT98App::run() {
    std::unique_ptr<Receiver> rx;
    std::vector<cf32> iq;
    double busy_ns = 0, span_ns = 0;
    while (!stop_) {
        auto d = sub_->pop(100ms);
        if (!d) continue;
        const auto& h = d->block.header();
        if (h.sample_count == 0) continue;
        if (rebuild_.exchange(false) || !rx || (core() && core()->source().config().center_freq != built_center_)) { if (core()) rx = build_receiver(*core()); }
        if (!rx) continue;
        const auto t0 = std::chrono::steady_clock::now();
        rx->set_squelch_db(squelch_db_);
        secret_drain_results();
        auto in = d->block.as<sc16>();
        iq.resize(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) iq[i] = cf32(in[i].real() / 32768.f, in[i].imag() / 32768.f);
        rx->process(iq, h.sample_index);
        mix_flush(h.sample_end());
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (int c = 0; c < cfg_.num_channels; ++c) {
                const auto& m = rx->metrics(c);
                auto& s = stats_[static_cast<std::size_t>(c)];
                s.power_db = m.power_db; s.open = m.open; s.frames = m.frames; s.sacch_ok = m.sacch_ok; s.pich_ok = m.pich_ok;
                s.syncs = m.sync_detections; s.best_sse = m.best_sse; s.sps = m.sps; s.csm = m.csm;
                auto& sc = secret_ch_[static_cast<std::size_t>(c)];
                if (!m.open && sc.tracker.active()) { sc.call_secret = false; sc.tracker.on_clear(); }   // squelch 閉 = 呼の終わり
                s.secret = sc.call_secret; s.key = sc.tracker.key(); s.secret_status = sc.tracker.status();
            }
        }
        // DSP 負荷: 処理時間 / ブロックの実時間(IIR)
        const double dt = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        busy_ns = 0.9 * busy_ns + 0.1 * dt;
        span_ns = 0.9 * span_ns + 0.1 * (static_cast<double>(h.sample_count) / kRfRate * 1e9);
        dsp_load_ = span_ns > 0 ? busy_ns / span_ns : 0;
    }
}

} // namespace spear::apps
