#include "adsb_app.hpp"
#include "spear/core/tap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace spear::apps {

using namespace std::chrono_literals;
using namespace spear::adsb;

namespace {
constexpr double kRfRate = 8e6;         // 4 サンプル/チップ(0.125 µs)。sc16 で 32 MB/s(M-1 で 10 Msps の損失 0 を確認済み)
constexpr double kAdsbHz = 1090e6;
constexpr std::size_t kTraceDecim = 1;  // 振幅窓は 240 チップ × 4 = 960 点をそのまま
}

AdsbApp::AdsbApp(appfw::AppInfo info, QObject* parent) : appfw::GuiApp(std::move(info), parent) {
    trace_.setCapacity(1);
    // 再起動をまたいで残す運転状態(現場固有値 ref/lo_offset は site.conf、選択機・観測値は宣言しない)
    persist({"sortKey", "fixBits", "rangeKm", "view.dbMin", "view.dbMax", "view.manualRange", "view.averaging"});
}
AdsbApp::~AdsbApp() = default;

void AdsbApp::configure(const QVariantMap& settings) {
    if (settings.contains("adsb.lo_offset_hz")) lo_offset_hz_ = settings["adsb.lo_offset_hz"].toDouble();
    if (settings.contains("adsb.ref_lat") && settings.contains("adsb.ref_lon"))
        ref_ = Position{settings["adsb.ref_lat"].toDouble(), settings["adsb.ref_lon"].toDouble()};
    Q_EMIT configChanged();
}

RfConfig AdsbApp::declare_rf_config() const {
    RfConfig c = GuiApp::declare_rf_config();
    c.sample_rate = kRfRate;
    c.bandwidth = kRfRate;
    c.center_freq = kAdsbHz - lo_offset_hz_;   // LO を 1090 MHz から離す。受信機の回転で戻す(振幅復調なので誤差は無関係)
    return c;
}

void AdsbApp::clearTable() { clear_ = true; }

QVariantList AdsbApp::aircraft() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<const Aircraft*> v;
    for (const auto& a : table_.all()) v.push_back(&a);
    const QString key = sort_key_;
    std::sort(v.begin(), v.end(), [&](const Aircraft* a, const Aircraft* b) {
        if (key == "seen") return a->last_seen_s > b->last_seen_s;
        if (key == "call") { if (a->callsign.empty() != b->callsign.empty()) return !a->callsign.empty(); return a->callsign < b->callsign; }
        if (key == "alt") return a->altitude_ft.value_or(-1) > b->altitude_ft.value_or(-1);
        // dist: 位置のある機を近い順、無い機は後ろ(最終受信順)
        if (a->distance_km.has_value() != b->distance_km.has_value()) return a->distance_km.has_value();
        if (a->distance_km) return *a->distance_km < *b->distance_km;
        return a->last_seen_s > b->last_seen_s;
    });
    QVariantList out;
    const double now = rate_hist_.empty() ? 0 : rate_hist_.back().first;
    for (const Aircraft* a : v) {
        QVariantMap m;
        m["icao"] = static_cast<int>(a->icao);
        m["hex"] = QString("%1").arg(a->icao, 6, 16, QChar('0')).toUpper();
        m["callsign"] = QString::fromStdString(a->callsign);
        m["squawk"] = a->squawk ? QString("%1").arg(*a->squawk, 4, 10, QChar('0')) : QString();
        m["alt"] = a->altitude_ft ? *a->altitude_ft : -1;
        m["gs"] = a->gs_kt ? *a->gs_kt : -1.0;
        m["trk"] = a->track_deg ? *a->track_deg : -1.0;
        m["heading"] = a->track_is_heading;
        m["vr"] = a->vr_fpm ? *a->vr_fpm : 0;
        m["hasVr"] = a->vr_fpm.has_value();
        m["hasPos"] = a->position.has_value();
        m["lat"] = a->position ? a->position->lat : 0.0;
        m["lon"] = a->position ? a->position->lon : 0.0;
        m["dist"] = a->distance_km ? *a->distance_km : -1.0;
        m["brg"] = a->bearing_deg ? *a->bearing_deg : -1.0;
        m["msgs"] = static_cast<double>(a->messages);
        m["age"] = std::max(0.0, now - a->last_seen_s);
        m["rssi"] = a->rssi_db;
        m["ground"] = a->on_ground;
        out.push_back(m);
    }
    return out;
}

QVariantMap AdsbApp::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    QVariantMap m;
    m["preambles"] = static_cast<double>(metrics_.preambles);
    m["frames"] = static_cast<double>(metrics_.frames);
    m["crcBad"] = static_cast<double>(metrics_.crc_bad);
    m["fixed"] = static_cast<double>(metrics_.fixed);
    m["apUnknown"] = static_cast<double>(metrics_.ap_unknown);
    m["knownIcao"] = static_cast<double>(metrics_.known_icao);
    m["df17"] = static_cast<double>(metrics_.by_df[17] + metrics_.by_df[18]);
    m["df11"] = static_cast<double>(metrics_.by_df[11]);
    m["dfAp"] = static_cast<double>(metrics_.by_df[0] + metrics_.by_df[4] + metrics_.by_df[5] + metrics_.by_df[16] + metrics_.by_df[20] + metrics_.by_df[21]);
    m["framesPerS"] = frames_per_s_;
    m["preamblesPerS"] = preambles_per_s_;
    m["aircraft"] = static_cast<double>(table_.size());
    m["withPos"] = static_cast<double>(std::count_if(table_.all().begin(), table_.all().end(), [](const Aircraft& a) { return a.position.has_value(); }));
    m["positions"] = static_cast<double>(table_.total_positions());
    m["rejected"] = static_cast<double>(table_.rejected_positions());
    m["drops"] = sub_ ? static_cast<double>(sub_->stats().dropped_blocks) : 0.0;
    m["dspLoad"] = dsp_load_.load();
    m["eventsNew"] = static_cast<double>(events_new_);
    m["eventsPos"] = static_cast<double>(events_pos_);
    m["eventsLost"] = static_cast<double>(events_lost_);
    return m;
}

std::unique_ptr<Receiver> AdsbApp::build_receiver(Core& core) {
    ReceiverConfig cfg;
    cfg.in_rate = kRfRate;
    // 回転量は Core の実 LO から導く(録音再生でも同じ経路)
    cfg.lo_offset_hz = kAdsbHz - core.source().config().center_freq;
    cfg.fix_single_bit = fix_bits_;
    auto rx = std::make_unique<Receiver>(cfg);
    Observer o;
    o.power = [](std::span<const float> pwr, uint64_t) { TAP("adsb.power", pwr.data(), pwr.size()); };
    o.frame = [this](const Frame& f, std::span<const float> window) {
        // 電力窓 → 振幅(正規化)→ TraceSource(容量 1 = 最後のフレーム)
        std::vector<float> tr(window.size() / kTraceDecim);
        float peak = 1e-20f;
        for (float v : window) peak = std::max(peak, v);
        const float inv = 1.f / std::sqrt(peak);
        for (std::size_t i = 0; i < tr.size(); ++i) tr[i] = std::sqrt(std::max(0.f, window[i * kTraceDecim])) * inv;
        trace_.push(tr);
        std::lock_guard<std::mutex> lk(mu_);
        table_.update(f);
        QVariantMap m;
        m["hex"] = QString::fromStdString(to_hex(f.bytes, f.nbits));
        m["df"] = f.msg.df;
        m["icao"] = QString("%1").arg(f.msg.icao, 6, 16, QChar('0')).toUpper();
        m["tc"] = f.msg.tc;
        m["rssi"] = f.rssi_db;
        m["snr"] = f.snr_db;
        m["fixed"] = f.fixed;
        m["index"] = static_cast<double>(f.input_sample_index);
        last_frame_ = m;
    };
    rx->set_observer(std::move(o));
    return rx;
}

void AdsbApp::on_start(Core& core) {
    auto& rx = core.rx();
    vp_ = std::make_unique<appfw::ViewProcessor>(rx, info().id + ".view");
    view_.setProcessor(vp_.get());
    vp_->start();
    {
        std::lock_guard<std::mutex> lk(mu_);
        table_ = AircraftTable(TableConfig{60, 10, ref_ ? 600.0 : 0.0});
        table_.set_reference(ref_);
        // 事象は新規機 / 初回位置 / 消失だけ(フレームごとに出すと都市部では 1000/s を超え、イベントログが溢れる)
        EventBus* events = &core.events();
        const std::string id = info().id;
        AircraftTable::Callbacks cb;
        cb.created = [this, events, id](const Aircraft& a, const Frame& f) {
            ++events_new_;
            char buf[64]; std::snprintf(buf, sizeof buf, "aircraft %06X new (DF%d)", a.icao, f.msg.df);
            events->emit(EventKind::Info, id, buf, {0, f.input_sample_index, f.input_sample_end}, static_cast<int64_t>(a.icao));
        };
        cb.first_position = [this, events, id](const Aircraft& a, const Frame& f) {
            ++events_pos_;
            char buf[128];
            std::snprintf(buf, sizeof buf, "aircraft %06X %s position %.4f %.4f%s", a.icao, a.callsign.c_str(), a.position->lat, a.position->lon,
                          a.distance_km ? (" " + std::to_string(static_cast<int>(*a.distance_km)) + " km").c_str() : "");
            events->emit(EventKind::Info, id, buf, {0, f.input_sample_index, f.input_sample_end}, static_cast<int64_t>(a.icao));
        };
        cb.lost = [this, events, id](const Aircraft& a) {
            ++events_lost_;
            char buf[64]; std::snprintf(buf, sizeof buf, "aircraft %06X %s lost (%llu msgs)", a.icao, a.callsign.c_str(), (unsigned long long)a.messages);
            events->emit(EventKind::Info, id, buf, {}, static_cast<int64_t>(a.icao));
        };
        table_.set_callbacks(cb);
        last_frame_.clear();
        metrics_ = Metrics{};
        rate_hist_.clear();
        frames_per_s_ = preambles_per_s_ = 0;
        events_new_ = events_pos_ = events_lost_ = 0;
    }
    trace_.clear();
    sub_ = rx.subscribe(info().id, DeliveryPolicy::Lossless, 64);
    stop_ = false;
    th_ = std::thread([this] { run(); });
    timer_ = startTimer(250);
}

void AdsbApp::on_stop() {
    if (timer_) { killTimer(timer_); timer_ = 0; }
    stop_ = true;
    if (th_.joinable()) th_.join();
    sub_.reset();
    view_.setProcessor(nullptr);
    vp_.reset();
    trace_.clear();
}

void AdsbApp::run() {
    std::unique_ptr<Receiver> rx;
    std::vector<cf32> iq;
    double busy_ns = 0, span_ns = 0;
    double built_center = 0, last_expire = 0;
    std::deque<std::pair<double, uint64_t>> pre_hist;
    while (!stop_) {
        auto d = sub_->pop(100ms);
        if (!d) continue;
        const auto& h = d->block.header();
        if (h.sample_count == 0) continue;
        if (!rx || (core() && core()->source().config().center_freq != built_center)) {
            if (!core()) continue;
            built_center = core()->source().config().center_freq;
            rx = build_receiver(*core());
        }
        const auto t0 = std::chrono::steady_clock::now();
        rx->set_fix_single_bit(fix_bits_);
        auto in = d->block.as<sc16>();
        iq.resize(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) iq[i] = cf32(in[i].real() / 32768.f, in[i].imag() / 32768.f);
        rx->process(iq, h.sample_index);
        const double now = rx->now_s();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (clear_.exchange(false)) table_.clear();
            if (now - last_expire > 1.0) { last_expire = now; table_.expire(now); }
            metrics_ = rx->metrics();
            // 直近 5 s のレート
            rate_hist_.emplace_back(now, metrics_.frames);
            pre_hist.emplace_back(now, metrics_.preambles);
            while (!rate_hist_.empty() && now - rate_hist_.front().first > 5.0) rate_hist_.pop_front();
            while (!pre_hist.empty() && now - pre_hist.front().first > 5.0) pre_hist.pop_front();
            const double dt = now - rate_hist_.front().first;
            frames_per_s_ = dt > 0.5 ? static_cast<double>(metrics_.frames - rate_hist_.front().second) / dt : 0;
            preambles_per_s_ = dt > 0.5 ? static_cast<double>(metrics_.preambles - pre_hist.front().second) / dt : 0;
        }
        const double dtn = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        busy_ns = 0.9 * busy_ns + 0.1 * dtn;
        span_ns = 0.9 * span_ns + 0.1 * (static_cast<double>(h.sample_count) / kRfRate * 1e9);
        dsp_load_ = span_ns > 0 ? busy_ns / span_ns : 0;
    }
}

} // namespace spear::apps
