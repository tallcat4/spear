#include "demod_app.hpp"
#include "spear/core/tap.hpp"
#include "spear/dsp/stage.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spear::apps {

using namespace std::chrono_literals;

namespace {
constexpr double kRfRate = 1.92e6;     // B210: 30.72 MHz / 16(整数比)
constexpr std::size_t kChanDecim = 8;  // → 240 kHz
constexpr std::size_t kAudioDecim = 5; // → 48 kHz
constexpr double kChanRate = kRfRate / kChanDecim;
constexpr double kAudioRate = kChanRate / kAudioDecim;

// 用途の定数(App 側。共通 DSP には置かない: docs/dsp-boundary.md)
struct ModeParams {
    double chan_cutoff_hz;   // channel filter(片側)
    double audio_cutoff_hz;  // audio LPF
    double deemph_tau_s;     // 0 = なし
    double fm_deviation_hz;  // FM 出力スケール用(0 = AM)
    double bandwidth_hz;     // 表示用(両側)
};
ModeParams params_for(const std::string& m) {
    if (m == "WFM") return {100e3, 15e3, 50e-6, 75e3, 200e3};   // 放送 FM(日本: de-emphasis 50 µs)
    if (m == "AM")  return {5e3, 4e3, 0, 0, 10e3};
    return {8e3, 3.4e3, 0, 2.5e3, 12.5e3};                       // NFM: 特定小電力 / 業務無線(12.5 kHz ch)
}

// ---- App 内の原始演算(2 つ目の App が要ったら共通へ抽出する候補)----
// quadrature demod: arg(x[n] · conj(x[n-1])) [rad/sample]
struct QuadDemod {
    cf32 prev{0.f, 0.f};
    float operator()(cf32 x) { const cf32 p = x * std::conj(prev); prev = x; return std::atan2(p.imag(), p.real()); }
};
// 単極 IIR 低域(de-emphasis / DC block に使う)
struct OnePole {
    float a = 0.f, y = 0.f;
    void set(double tau_s, double rate) { a = tau_s > 0 ? static_cast<float>(std::exp(-1.0 / (tau_s * rate))) : 0.f; }
    float lp(float x) { y = a * y + (1.f - a) * x; return y; }
};
} // namespace

struct DemodApp::Chain {
    ModeParams p;
    double offset_hz = 0;
    dsp::FirDecimator<cf32> chan;      // 1.92 M → 240 k(mode 共通の粗い LPF)
    dsp::FirDecimator<cf32> chan2;     // 240 k → 240 k(mode の channel filter)
    dsp::FirDecimator<float> audio;    // 240 k → 48 k
    QuadDemod quad;
    OnePole deemph, dc;
    dsp::Provenance prov_chan, prov_audio;   // 出力 index → radio.rx の sample index
    double mix_phase = 0, mix_step = 0;
    float fm_scale = 1.f, am_scale = 1.f;
};

DemodApp::DemodApp(appfw::AppInfo info, QObject* parent) : appfw::GuiApp(std::move(info), parent) {
    // 再起動をまたいで残す運転状態。channelOffsetHz は loOffset から導く派生値、RX 周波数は Core(草案はシェルが残す)
    persist({"mode", "loOffsetHz", "squelchDb", "volume", "mute", "view.dbMin", "view.dbMax", "view.manualRange", "view.averaging",
             "channelView.dbMin", "channelView.dbMax", "channelView.manualRange", "channelView.averaging"});
}
DemodApp::~DemodApp() = default;

void DemodApp::configure(const QVariantMap& settings) {
    if (settings.contains("audio_device")) audio_device_ = settings["audio_device"].toString().toStdString();
}

RfConfig DemodApp::declare_rf_config() const {
    RfConfig c = GuiApp::declare_rf_config();
    c.sample_rate = kRfRate;            // 音声 48 kHz へ整数比で落とせる rate を App が決める
    c.bandwidth = kRfRate;
    // シェルの草案 center を「聞きたい周波数」と解釈し、LO をそこから lo_offset 離す
    const double max_off = kRfRate / 2 - params_for(mode_).bandwidth_hz;
    const double off = std::clamp(lo_offset_hz_, -max_off, max_off);
    const_cast<DemodApp*>(this)->offset_hz_ = off;
    c.center_freq -= off;
    return c;
}

void DemodApp::setMode(const QString& m) {
    const std::string v = m.toStdString();
    if (v != "WFM" && v != "NFM" && v != "AM") return;
    mode_ = v;
    rebuild_ = true;
    Q_EMIT modeChanged();
}

double DemodApp::rxFreq() const {
    const double center = core() ? core()->source().config().center_freq : GuiApp::declare_rf_config().center_freq;
    return center + offset_hz_;
}

void DemodApp::tuneRx(double rx_hz) {
    // LO はチャネルから lo_offset 離す。offset は span 内(±(rate/2 − bw))に収める
    const double max_off = kRfRate / 2 - params_for(mode_).bandwidth_hz;
    const double off = std::clamp(lo_offset_hz_, -max_off, max_off);
    offset_hz_ = off;
    rebuild_ = true;
    if (core()) core()->source().retune(rx_hz - off);
    else { RfConfig c = GuiApp::declare_rf_config(); c.center_freq = rx_hz - off; set_rf_config(c); }
    Q_EMIT modeChanged();
}

void DemodApp::setLoOffsetHz(double hz) {
    const double rx = rxFreq();
    lo_offset_hz_ = hz;
    tuneRx(rx);   // RX 周波数を保ったまま LO を移す
}

void DemodApp::cycleMode() {
    setMode(mode_ == "NFM" ? "WFM" : (mode_ == "WFM" ? "AM" : "NFM"));
}

void DemodApp::setVolume(double v) {
    volume_ = std::clamp(v, 0.0, 1.0);
    if (audio_) audio_->set_volume(static_cast<float>(volume_));
    Q_EMIT audioChanged();
}

void DemodApp::setMute(bool m) {
    mute_ = m;
    if (audio_) audio_->set_mute(m);
    Q_EMIT audioChanged();
}

double DemodApp::channelBandwidth() const { return params_for(mode_).bandwidth_hz; }

std::unique_ptr<DemodApp::Chain> DemodApp::build_chain(double in_rate) {
    auto c = std::make_unique<Chain>();
    c->p = params_for(mode_);
    c->offset_hz = offset_hz_;
    c->chan  = dsp::FirDecimator<cf32>("chan_decim", in_rate, dsp::design_lowpass(in_rate, 110e3, 63), kChanDecim);
    c->chan2 = dsp::FirDecimator<cf32>("chan_filter", kChanRate, dsp::design_lowpass(kChanRate, c->p.chan_cutoff_hz, 127), 1);
    c->audio = dsp::FirDecimator<float>("audio_decim", kChanRate, dsp::design_lowpass(kChanRate, c->p.audio_cutoff_hz, 95), kAudioDecim);
    c->deemph.set(c->p.deemph_tau_s, kAudioRate);
    c->dc.set(0.05, kChanRate);   // AM の DC(搬送波)除去: 50 ms
    c->mix_step = -2.0 * std::numbers::pi * c->offset_hz / in_rate;
    // FM: rad/sample → deviation で 1.0 に正規化。AM: 包絡線の変動分をおおよそ 1.0 に
    c->fm_scale = c->p.fm_deviation_hz > 0 ? static_cast<float>(kChanRate / (2.0 * std::numbers::pi * c->p.fm_deviation_hz)) : 0.f;
    c->am_scale = 4.f;
    // Provenance: 段が宣言した rate 比と群遅延から合成(§4.5 の手計算を規約で置き換える)
    c->prov_chan = dsp::Provenance::identity().then(c->chan.info()).then(c->chan2.info());
    c->prov_audio = c->prov_chan.then(c->audio.info());
    dsp_delay_ms_ = c->prov_audio.offset / in_rate * 1e3;
    return c;
}

void DemodApp::on_start(Core& core) {
    auto& rx = core.rx();
    const double center = core.source().config().center_freq;
    // ---- App 内部の stream(Stream Bus)。producer(DSP thread)は consumer を知らない ----
    channel_pool_ = std::make_unique<BlockPool>(4096 * sizeof(cf32), 64);
    audio_pool_   = std::make_unique<BlockPool>(2048 * sizeof(float), 64);
    channel_ = std::make_unique<Stream<cf32>>(StreamMeta{"demod.channel", DataType::ComplexFloat32, kChanRate, center + offset_hz_, params_for(mode_).bandwidth_hz, "FS", Direction::RX}, &core.events());
    audio_stream_ = std::make_unique<Stream<float>>(StreamMeta{"demod.audio", DataType::Float32, kAudioRate, 0, kAudioRate / 2, "FS", Direction::RX}, &core.events());
    chan_seq_ = chan_index_ = audio_seq_ = audio_index_ = 0;

    // ---- 観測点: radio.rx と demod.channel に waterfall(DSP コードには触れていない)----
    vp_ = std::make_unique<appfw::ViewProcessor>(rx, info().id + ".view");
    view_.setProcessor(vp_.get());
    vp_->start();
    channel_vp_ = std::make_unique<appfw::ViewProcessor>(*channel_, info().id + ".channel_view", 512, 512);
    channel_view_.setProcessor(channel_vp_.get());
    channel_vp_->start();

    // ---- 音声 ----
    audio_ = std::make_unique<AudioSink>(&core.events(), audio_device_, static_cast<unsigned>(kAudioRate));
    audio_->set_volume(static_cast<float>(volume_));
    audio_->set_mute(mute_);
    std::string err;
    if (!audio_->open(&err)) core.events().emit(EventKind::Warning, info().id, "audio disabled: " + err);

    // ---- DSP thread(Lossless: 復調器は全 sample が要る)----
    sub_ = rx.subscribe(info().id, DeliveryPolicy::Lossless, 64);
    rebuild_ = true;
    stop_ = false;
    th_ = std::thread([this] { run(); });
    timer_ = startTimer(100);
}

void DemodApp::on_stop() {
    if (timer_) { killTimer(timer_); timer_ = 0; }
    stop_ = true;
    if (th_.joinable()) th_.join();
    sub_.reset();
    channel_view_.setProcessor(nullptr);
    view_.setProcessor(nullptr);
    channel_vp_.reset();
    vp_.reset();
    audio_.reset();
    if (channel_) channel_->end();
    if (audio_stream_) audio_stream_->end();
    channel_.reset();
    audio_stream_.reset();
    channel_pool_.reset();
    audio_pool_.reset();
}

void DemodApp::timerEvent(QTimerEvent*) { Q_EMIT metersChanged(); }

void DemodApp::run() {
    std::unique_ptr<Chain> c;
    std::vector<cf32> iq, chan1, chan2;
    std::vector<float> disc, audio;
    bool carrier = false;
    uint64_t carrier_since = 0;
    Flags pending;
    while (!stop_) {
        auto d = sub_->pop(100ms);
        if (!d) continue;
        const auto& h = d->block.header();
        if (h.sample_count == 0) continue;
        if (rebuild_.exchange(false) || !c) c = build_chain(kRfRate);
        if (d->flags.has(Flag::Discontinuity)) pending.set(Flag::Discontinuity);

        // 1) sc16 → cf32、周波数シフト(選局)
        auto in = d->block.as<sc16>();
        iq.resize(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            const cf32 x(in[i].real() / 32768.f, in[i].imag() / 32768.f);
            if (c->mix_step != 0.0) {
                const cf32 lo(static_cast<float>(std::cos(c->mix_phase)), static_cast<float>(std::sin(c->mix_phase)));
                iq[i] = x * lo;
                c->mix_phase += c->mix_step;
                if (c->mix_phase > 2 * std::numbers::pi) c->mix_phase -= 2 * std::numbers::pi;
                else if (c->mix_phase < -2 * std::numbers::pi) c->mix_phase += 2 * std::numbers::pi;
            } else {
                iq[i] = x;
            }
        }
        // 2) channelizer(共通 DSP)
        chan1.resize(iq.size() / kChanDecim + 1);
        const std::size_t n1 = c->chan.process(iq, chan1);
        chan2.resize(n1 + 1);
        const std::size_t n2 = c->chan2.process(std::span<const cf32>(chan1.data(), n1), chan2);
        TAP("demod.channel", chan2.data(), n2);
        // 2b) channel IQ を Stream Bus へ(観測点。誰が見ているかは知らない)
        {
            auto b = channel_pool_->acquire<cf32>(n2);
            std::copy_n(chan2.data(), n2, b.data<cf32>().data());
            auto& ch = b.header();
            ch.generation = h.generation; ch.sequence = chan_seq_++; ch.sample_index = chan_index_;
            ch.hw_time = h.hw_time; ch.host_ns = h.host_ns; ch.flags = pending;
            chan_index_ += n2;
            channel_->publish(b.commit(static_cast<uint32_t>(n2)));
        }
        // 3) 信号レベルと squelch(channel 電力)
        double pw = 0;
        for (std::size_t i = 0; i < n2; ++i) pw += std::norm(chan2[i]);
        const double sig_db = 10.0 * std::log10(pw / static_cast<double>(std::max<std::size_t>(n2, 1)) + 1e-20);
        signal_db_ = sig_db;
        const bool open = sig_db >= squelch_db_;
        squelch_open_ = open;
        if (open != carrier) {
            // 搬送波の出現/消失を event に。範囲は Provenance で radio.rx の sample index に逆算(§4.5)
            const uint64_t in_idx = static_cast<uint64_t>(std::llround(c->prov_chan.input_index(static_cast<double>(chan_index_ - n2))));
            if (core()) core()->events().emit(open ? EventKind::Info : EventKind::Info, info().id,
                                              open ? "carrier detected" : "carrier lost",
                                              {h.generation, open ? in_idx : carrier_since, in_idx}, static_cast<int64_t>(std::lround(sig_db)));
            carrier = open;
            carrier_since = in_idx;
        }
        // 4) 復調(App 内の原始演算)
        disc.resize(n2);
        if (c->p.fm_deviation_hz > 0) {
            for (std::size_t i = 0; i < n2; ++i) disc[i] = c->quad(chan2[i]) * c->fm_scale;
        } else {
            for (std::size_t i = 0; i < n2; ++i) { const float m = std::abs(chan2[i]); disc[i] = (m - c->dc.lp(m)) * c->am_scale; }
        }
        TAP("demod.discriminator", disc.data(), n2);
        // 5) 音声 LPF + 間引き(共通 DSP)、de-emphasis(App)、squelch
        audio.resize(n2 / kAudioDecim + 1);
        const std::size_t na = c->audio.process(std::span<const float>(disc.data(), n2), audio);
        double apw = 0;
        for (std::size_t i = 0; i < na; ++i) {
            float v = c->p.deemph_tau_s > 0 ? c->deemph.lp(audio[i]) : audio[i];
            if (!open) v = 0.f;
            audio[i] = std::clamp(v, -1.f, 1.f);
            apw += audio[i] * audio[i];
        }
        audio_db_ = 10.0 * std::log10(apw / static_cast<double>(std::max<std::size_t>(na, 1)) + 1e-20);
        TAP("demod.audio", audio.data(), na);
        {
            auto b = audio_pool_->acquire<float>(na);
            std::copy_n(audio.data(), na, b.data<float>().data());
            auto& ah = b.header();
            ah.generation = h.generation; ah.sequence = audio_seq_++; ah.sample_index = audio_index_;
            ah.hw_time = h.hw_time; ah.host_ns = h.host_ns; ah.flags = pending;
            audio_index_ += na;
            audio_stream_->publish(b.commit(static_cast<uint32_t>(na)));
        }
        if (audio_) audio_->write(std::span<const float>(audio.data(), na));
        pending = {};
    }
}

} // namespace spear::apps
