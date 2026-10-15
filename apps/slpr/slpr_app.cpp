#include "slpr_app.hpp"
#include "spear/core/tap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace spear::apps {

using namespace std::chrono_literals;
using namespace spear::slpr;

namespace {
constexpr double kRfRate = 4e6;   // 4 Msps → resamp1 ÷10 → 400 kHz = 32 × 12.5 kHz(STD-T98 と同じ RF 構成。実機で drop 0 の実績)
}

SlprApp::SlprApp(appfw::AppInfo info, QObject* parent) : appfw::GuiApp(std::move(info), parent) {
    stats_.resize(static_cast<std::size_t>(kNumChannels));
    // 再起動をまたいで残す運転状態(個体値は site.conf、チャネル IQ 表示のレンジは帯域表示に追従する派生値なので宣言しない)
    persist({"squelchMarginDb", "selectedChannel", "mute", "allChannelAudio", "bandView.dbMin", "bandView.dbMax", "bandView.manualRange", "bandView.averaging"});
}
SlprApp::~SlprApp() = default;

void SlprApp::configure(const QVariantMap& settings) {
    if (settings.contains("audio_device")) audio_device_ = settings["audio_device"].toString().toStdString();
    if (settings.contains("slpr.band_center_hz")) band_center_hz_ = settings["slpr.band_center_hz"].toDouble();
    if (settings.contains("slpr.squelch_margin_db")) squelch_margin_db_ = settings["slpr.squelch_margin_db"].toDouble();
}

RfConfig SlprApp::declare_rf_config() const {
    RfConfig c = GuiApp::declare_rf_config();
    c.sample_rate = kRfRate;
    c.bandwidth = kRfRate;
    c.center_freq = band_center_hz_ - lo_offset_hz_;   // LO を帯域中心から離す。帯域は Receiver の回転で戻す
    return c;
}

void SlprApp::setSelectedChannel(int ch) {
    ch = std::clamp(ch, 0, kNumChannels - 1);
    if (ch == selected_) return;
    selected_ = ch;
    Q_EMIT configChanged();
}

void SlprApp::setMute(bool m) { mute_ = m; if (audio_) audio_->set_mute(m); Q_EMIT configChanged(); }

QVariantList SlprApp::channels() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantList out;
    for (int i = 0; i < kNumChannels; ++i) {
        const auto& s = stats_[static_cast<std::size_t>(i)];
        const auto& d = kChannels[static_cast<std::size_t>(i)];
        QVariantMap m;
        m["ch"] = d.number;
        m["name"] = QString::fromLatin1(d.name);
        m["freq"] = d.freq_hz;
        m["group"] = QString::fromLatin1(d.group);
        m["power"] = s.power_db;
        m["floor"] = s.floor_db;
        m["threshold"] = s.threshold_db;
        m["open"] = s.open;
        m["audioDb"] = s.audio_db;
        m["opens"] = static_cast<double>(s.opens);
        m["openS"] = s.open_s;
        out.push_back(m);
    }
    return out;
}

QVariantMap SlprApp::selected() const {
    std::lock_guard<std::mutex> lk(mu_);
    const int i = selected_.load();
    const auto& s = stats_[static_cast<std::size_t>(i)];
    const auto& d = kChannels[static_cast<std::size_t>(i)];
    QVariantMap m;
    m["ch"] = d.number;
    m["name"] = QString::fromLatin1(d.name);
    m["freq"] = d.freq_hz;
    m["group"] = QString::fromLatin1(d.group);
    m["power"] = s.power_db;
    m["floor"] = s.floor_db;
    m["threshold"] = s.threshold_db;
    m["open"] = s.open;
    m["audioDb"] = s.audio_db;
    m["opens"] = static_cast<double>(s.opens);
    m["openS"] = s.open_s;
    return m;
}

int SlprApp::openChannels() const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (const auto& s : stats_) n += s.open ? 1 : 0;
    return n;
}

std::unique_ptr<Receiver> SlprApp::build_receiver(Core& core) {
    ReceiverConfig c;
    c.in_rate = kRfRate;
    // 帯域中心を DC に戻す回転 = 帯域中心 − 実際の LO。実際の LO は Core(単一の所有者)から読む:
    // B210 なら band − loOffset、録音再生なら録音時の中心(retune できない)なのでオフセットは自然に 0 になる。
    // 個体の LO 誤差はここでは扱わない(Core の Radio が radio.freq_err_ppm で LO 側で打ち消す)
    built_center_ = core.source().config().center_freq;
    c.freq_err_hz = band_center_hz_ - built_center_;
    c.squelch_margin_db = squelch_margin_db_;
    auto rx = std::make_unique<Receiver>(c);
    Observer o;
    o.band = [this](std::span<const cf32> iq, uint64_t) {
        TAP("slpr.band", iq.data(), iq.size());
        publish(*band_, *band_pool_, iq, band_seq_, band_index_);
    };
    o.channel_iq = [this](int ch, std::span<const cf32> iq, uint64_t) {
        if (ch != selected_) return;
        publish(*channel_, *channel_pool_, iq, chan_seq_, chan_index_);
    };
    o.audio = [this](int ch, std::span<const float> a, uint64_t) {
        // ミックス: 全チャネル同時再生か、選択チャネルだけ。閉じたチャネルは受信機が 0 を出しているので足しても変わらない
        if (!(all_audio_ || ch == selected_)) return;
        if (mix_.size() < a.size()) mix_.resize(a.size(), 0.f);
        for (std::size_t i = 0; i < a.size(); ++i) mix_[i] += a[i];
    };
    o.squelch = [this, &core](int ch, bool open, uint64_t input_index) {
        // 開閉 event(provenance: 開は先頭 index、閉は開いていた範囲)。1 送信 = 2 event で、イベントログを溢れさせない
        uint64_t since = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto& s = stats_[static_cast<std::size_t>(ch)];
            if (open) s.open_index = input_index;
            since = s.open_index;
        }
        const std::string name = kChannels[static_cast<std::size_t>(ch)].name;
        core.events().emit(EventKind::Info, info().id, open ? "ch" + name + " carrier" : "ch" + name + " clear",
                           {cur_generation_, open ? input_index : since, input_index}, kChannels[static_cast<std::size_t>(ch)].number);
    };
    rx->set_observer(std::move(o));
    return rx;
}

template <class T>
void SlprApp::publish(Stream<T>& s, BlockPool& pool, std::span<const T> x, uint64_t& seq, uint64_t& index) {
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

void SlprApp::on_start(Core& core) {
    auto& rx = core.rx();
    const double band_rate = 32 * kSpacingHz;
    band_pool_ = std::make_unique<BlockPool>(4096 * sizeof(cf32), 64);
    channel_pool_ = std::make_unique<BlockPool>(2048 * sizeof(cf32), 64);
    audio_pool_ = std::make_unique<BlockPool>(2048 * sizeof(float), 64);
    band_ = std::make_unique<Stream<cf32>>(StreamMeta{"slpr.band", DataType::ComplexFloat32, band_rate, band_center_hz_, band_rate, "FS", Direction::RX}, &core.events());
    channel_ = std::make_unique<Stream<cf32>>(StreamMeta{"slpr.channel", DataType::ComplexFloat32, kSpacingHz, channelFreqHz(selected_), kSpacingHz, "FS", Direction::RX}, &core.events());
    audio_stream_ = std::make_unique<Stream<float>>(StreamMeta{"slpr.audio", DataType::Float32, kSpacingHz, 0, kSpacingHz / 2, "FS", Direction::RX}, &core.events());
    band_seq_ = band_index_ = chan_seq_ = chan_index_ = audio_seq_ = audio_index_ = 0;

    vp_ = std::make_unique<appfw::ViewProcessor>(rx, info().id + ".view");
    view_.setProcessor(vp_.get());
    vp_->start();
    band_vp_ = std::make_unique<appfw::ViewProcessor>(*band_, info().id + ".band_view", 1024, 512);
    band_view_.setProcessor(band_vp_.get());
    band_vp_->start();
    channel_vp_ = std::make_unique<appfw::ViewProcessor>(*channel_, info().id + ".channel_view", 256, 256);
    channel_view_.setProcessor(channel_vp_.get());
    channel_vp_->start();

    // 音声は 12.5 kHz のまま(PFB の出力レート)。ALSA/PipeWire 側でリサンプルさせる(STD-T98 の 8 kHz と同じ扱い)
    audio_ = std::make_unique<AudioSink>(&core.events(), audio_device_, static_cast<unsigned>(kSpacingHz));
    audio_->set_mute(mute_);
    std::string err;
    if (!audio_->open(&err)) core.events().emit(EventKind::Warning, info().id, "audio disabled: " + err);

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : stats_) s = ChanStat{};
    }
    mix_.clear();
    mix_db_ = -999;
    sub_ = rx.subscribe(info().id, DeliveryPolicy::Lossless, 64);
    rebuild_ = true;
    stop_ = false;
    th_ = std::thread([this] { run(); });
    timer_ = startTimer(200);
}

void SlprApp::on_stop() {
    if (timer_) { killTimer(timer_); timer_ = 0; }
    stop_ = true;
    if (th_.joinable()) th_.join();
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
    if (audio_stream_) audio_stream_->end();
    band_.reset();
    channel_.reset();
    audio_stream_.reset();
    band_pool_.reset();
    channel_pool_.reset();
    audio_pool_.reset();
}

void SlprApp::timerEvent(QTimerEvent*) {
    // チャネル IQ 表示は帯域表示と同じ dBFS スケール(PFB の利得 1)なので、レンジを帯域表示に追従させる(所有者は帯域側)
    if (band_vp_ && channel_vp_) {
        float lo, hi, clo, chi;
        band_vp_->get_db_range(lo, hi);
        channel_vp_->get_db_range(clo, chi);
        if (lo != clo || hi != chi) { channel_vp_->set_db_range(lo, hi); Q_EMIT channel_view_.rangeChanged(); }
    }
    Q_EMIT metersChanged();
}

void SlprApp::run() {
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
        cur_generation_ = h.generation;
        rx->set_squelch_margin_db(squelch_margin_db_);
        auto in = d->block.as<sc16>();
        iq.resize(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) iq[i] = cf32(in[i].real() / 32768.f, in[i].imag() / 32768.f);
        mix_.clear();
        rx->process(iq, h.sample_index);
        // ミックス済み音声 → クランプ → Stream Bus(観測点)→ AudioSink
        double mpw = 0;
        for (float& v : mix_) { v = std::clamp(v, -1.f, 1.f); mpw += static_cast<double>(v) * v; }
        if (!mix_.empty()) {
            mix_db_ = 10.0 * std::log10(mpw / static_cast<double>(mix_.size()) + 1e-20);
            TAP("slpr.audio", mix_.data(), mix_.size());
            publish(*audio_stream_, *audio_pool_, std::span<const float>(mix_.data(), mix_.size()), audio_seq_, audio_index_);
            if (audio_) audio_->write(mix_);
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (int c = 0; c < kNumChannels; ++c) {
                const auto& m = rx->metrics(c);
                auto& s = stats_[static_cast<std::size_t>(c)];
                s.power_db = m.power_db; s.floor_db = m.floor_db; s.threshold_db = m.threshold_db; s.open = m.open;
                s.audio_db = m.audio_db; s.opens = m.opens; s.open_s = m.open_s;
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
