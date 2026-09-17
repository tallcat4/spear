// App: ADS-B(1090 MHz Mode S / Extended Squitter)— docs/apps/adsb.md
//
// 純 C++ の Receiver + AircraftTable を GUI に載せる。検証対象:
//   * radio.rx を 8 Msps のままフルレートで Lossless 消費する初めての App(間引かない。DSP 負荷と drops を見る)
//   * §4.5 provenance: フレーム event の sample 範囲を radio.rx index で(高頻度なので event は新規機 / 初回位置 / 消失だけ)
//   * 現場固有値(基準位置 adsb.ref_lat / ref_lon、LO オフセット)を site.conf で渡す経路
//   * 表と極座標という新しい表示(App 内に置き、2 つ目の利用者が出たら部品へ)
// RF: 8 Msps(4 サンプル/チップ)、LO = 1090 MHz − loOffset(ゼロ IF の DC スパイクを信号帯域の外へ)。振幅復調なので LO 誤差は無関係。
// GAIN はメニュー(固定利得を推奨: AGC はパルス信号に不向き)。
#pragma once

#include "aircraft.hpp"
#include "receiver.hpp"
#include "spear/appfw/app.hpp"
#include "spear/appfw/trace_source.hpp"
#include "spear/appfw/view_source.hpp"
#include "spear/core/stream.hpp"

#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace spear::apps {

class AdsbApp final : public appfw::GuiApp {
    Q_OBJECT
    // ---- この App が所有する State ----
    Q_PROPERTY(double loOffsetHz READ loOffsetHz NOTIFY configChanged)            // site.conf: adsb.lo_offset_hz
    Q_PROPERTY(bool hasReference READ hasReference NOTIFY configChanged)          // site.conf: adsb.ref_lat / adsb.ref_lon
    Q_PROPERTY(double refLat READ refLat NOTIFY configChanged)
    Q_PROPERTY(double refLon READ refLon NOTIFY configChanged)
    Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY configChanged)   // dist / seen / call / alt
    Q_PROPERTY(bool fixBits READ fixBits WRITE setFixBits NOTIFY configChanged)      // DF17/18 の 1 bit 訂正
    Q_PROPERTY(double rangeKm READ rangeKm WRITE setRangeKm NOTIFY configChanged)    // 極座標の外周
    Q_PROPERTY(int selectedIcao READ selectedIcao WRITE setSelectedIcao NOTIFY configChanged)   // 表でタップした機(0 = なし)
    // ---- 観測値(DSP thread が更新、timer で通知)----
    Q_PROPERTY(QVariantList aircraft READ aircraft NOTIFY metersChanged)
    Q_PROPERTY(QVariantMap stats READ stats NOTIFY metersChanged)
    Q_PROPERTY(QVariantMap lastFrame READ lastFrame NOTIFY metersChanged)
    // ---- 観測点 ----
    Q_PROPERTY(spear::appfw::ViewSource* view READ view CONSTANT)          // radio.rx
    Q_PROPERTY(spear::appfw::TraceSource* trace READ trace CONSTANT)       // 最後に受理したフレームの振幅窓(120 µs、正規化)
public:
    explicit AdsbApp(appfw::AppInfo info, QObject* parent = nullptr);
    ~AdsbApp() override;
    void configure(const QVariantMap& settings) override;
    RfConfig declare_rf_config() const override;

    double loOffsetHz() const { return lo_offset_hz_; }
    bool hasReference() const { return ref_.has_value(); }
    double refLat() const { return ref_ ? ref_->lat : 0; }
    double refLon() const { return ref_ ? ref_->lon : 0; }
    QString sortKey() const { return sort_key_; }
    void setSortKey(const QString& k) { sort_key_ = k; Q_EMIT configChanged(); }
    bool fixBits() const { return fix_bits_; }
    void setFixBits(bool on) { fix_bits_ = on; Q_EMIT configChanged(); }
    double rangeKm() const { return range_km_; }
    void setRangeKm(double km) { range_km_ = km; Q_EMIT configChanged(); }
    int selectedIcao() const { return selected_icao_; }
    void setSelectedIcao(int icao) { selected_icao_ = icao; Q_EMIT configChanged(); }
    Q_INVOKABLE void clearTable();

    QVariantList aircraft() const;
    QVariantMap stats() const;
    QVariantMap lastFrame() const { std::lock_guard<std::mutex> lk(mu_); return last_frame_; }
    appfw::ViewSource* view() { return &view_; }
    appfw::TraceSource* trace() { return &trace_; }

Q_SIGNALS:
    void configChanged();
    void metersChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override { Q_EMIT metersChanged(); }

private:
    void run();
    std::unique_ptr<adsb::Receiver> build_receiver(Core& core);

    double lo_offset_hz_ = 2e6;
    std::optional<adsb::Position> ref_;
    QString sort_key_ = "dist";
    std::atomic<bool> fix_bits_{true};
    double range_km_ = 200;
    int selected_icao_ = 0;
    std::atomic<bool> clear_{false};

    // DSP thread が書き、GUI thread が読む
    mutable std::mutex mu_;
    adsb::AircraftTable table_;
    QVariantMap last_frame_;
    adsb::Metrics metrics_;
    std::deque<std::pair<double, uint64_t>> rate_hist_;   // (t, frames) 直近 5 s のフレームレート用
    double frames_per_s_ = 0, preambles_per_s_ = 0;
    uint64_t events_new_ = 0, events_pos_ = 0, events_lost_ = 0;
    std::atomic<double> dsp_load_{0};

    appfw::ViewSource view_;
    appfw::TraceSource trace_;
    std::unique_ptr<appfw::ViewProcessor> vp_;
    std::shared_ptr<Subscription> sub_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    int timer_ = 0;
};

} // namespace spear::apps
