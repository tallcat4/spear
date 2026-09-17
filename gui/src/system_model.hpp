// spear-gui — SystemModel: Core の State / Event を QML へ (要件 §12.1 装置上の可観測性)
//
// 250 ms ごとに Core を読んで property を更新する(GUI が止まっても Core には影響しない)。
// Event は EventBus の listener から GUI thread へ queued で渡す。
#pragma once

#include "spear/core/core.hpp"

#include <QAbstractListModel>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <deque>

namespace spear::gui {

class EventListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { TimeRole = Qt::UserRole + 1, KindRole, SourceRole, DetailRole, SeverityRole, RangeRole };
    explicit EventListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& = {}) const override { return static_cast<int>(rows_.size()); }
    QVariant data(const QModelIndex& idx, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void push(const Event& e); // GUI thread
private:
    struct Row { QString time, kind, source, detail, range; int severity; };
    std::deque<Row> rows_;
    static constexpr int kMax = 400;
};

class SystemModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("created by the application")
    // ---- 装置状態 ----
    Q_PROPERTY(QString deviceState READ deviceState NOTIFY changed)
    Q_PROPERTY(int deviceStateCode READ deviceStateCode NOTIFY changed)
    Q_PROPERTY(QString deviceEvidence READ deviceEvidence NOTIFY changed)
    Q_PROPERTY(QString serial READ serial NOTIFY changed)
    Q_PROPERTY(QString fx3State READ fx3State NOTIFY changed)
    Q_PROPERTY(int stage READ stage NOTIFY changed)
    Q_PROPERTY(int generation READ generation NOTIFY changed)
    Q_PROPERTY(double stateSeconds READ stateSeconds NOTIFY changed)
    Q_PROPERTY(int deviceProgress READ deviceProgress NOTIFY changed)          // FPGA 書き込みの進捗 %(無ければ −1)
    Q_PROPERTY(bool warmingUp READ warmingUp NOTIFY changed)                   // 立ち上げ(Source::warm_up)進行中
    Q_PROPERTY(QStringList startupReport READ startupReport NOTIFY changed)   // 立ち上げの経過(自己診断 / 装置 / FPGA / tune)
    // ---- RF ----
    Q_PROPERTY(double centerFreq READ centerFreq NOTIFY changed)
    Q_PROPERTY(double appliedFreq READ appliedFreq NOTIFY changed)
    Q_PROPERTY(int stateVersion READ stateVersion NOTIFY changed)
    Q_PROPERTY(double sampleRate READ sampleRate NOTIFY changed)
    Q_PROPERTY(double gain READ gain NOTIFY changed)
    Q_PROPERTY(bool agc READ agc NOTIFY changed)
    Q_PROPERTY(QString antenna READ antenna NOTIFY changed)
    // ---- センサ ----
    Q_PROPERTY(bool sensorsValid READ sensorsValid NOTIFY changed)
    Q_PROPERTY(bool loLocked READ loLocked NOTIFY changed)
    Q_PROPERTY(double rxTemp READ rxTemp NOTIFY changed)
    Q_PROPERTY(double rssi READ rssi NOTIFY changed)
    Q_PROPERTY(double driftPpm READ driftPpm NOTIFY changed)
    Q_PROPERTY(double driftUncertaintyPpm READ driftUncertaintyPpm NOTIFY changed)
    // ---- 受信統計(event 集計)----
    Q_PROPERTY(int overflowCount READ overflowCount NOTIFY changed)
    Q_PROPERTY(int oosCount READ oosCount NOTIFY changed)
    Q_PROPERTY(int timeoutCount READ timeoutCount NOTIFY changed)
    Q_PROPERTY(int discontinuityCount READ discontinuityCount NOTIFY changed)
    Q_PROPERTY(int consumerOverflowCount READ consumerOverflowCount NOTIFY changed)
    Q_PROPERTY(QVariantList consumers READ consumers NOTIFY changed)
    // ---- host ----
    Q_PROPERTY(double cpuMhz READ cpuMhz NOTIFY changed)
    Q_PROPERTY(QString governor READ governor NOTIFY changed)
    Q_PROPERTY(double pkgTemp READ pkgTemp NOTIFY changed)
    Q_PROPERTY(int throttleCount READ throttleCount NOTIFY changed)
    Q_PROPERTY(bool onAc READ onAc NOTIFY changed)
    Q_PROPERTY(double diskFreeGb READ diskFreeGb NOTIFY changed)
    Q_PROPERTY(double rssMb READ rssMb NOTIFY changed)
    Q_PROPERTY(double memAvailMb READ memAvailMb NOTIFY changed)
    Q_PROPERTY(int rtprioLimit READ rtprioLimit NOTIFY changed)
    Q_PROPERTY(double load1 READ load1 NOTIFY changed)
    // ---- App ----
    Q_PROPERTY(QString activeApp READ activeApp NOTIFY changed)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY changed)
    Q_PROPERTY(QString clock READ clock NOTIFY changed)
    Q_PROPERTY(EventListModel* events READ events CONSTANT)
public:
    explicit SystemModel(Core& core, QObject* parent = nullptr);
    ~SystemModel() override;

    QString deviceState() const { return QString::fromUtf8(std::string(to_string(ds_.state)).c_str()); }
    int deviceStateCode() const { return static_cast<int>(ds_.state); }
    QString deviceEvidence() const { return QString::fromStdString(ds_.evidence); }
    QString serial() const { return QString::fromStdString(ds_.serial); }
    QString fx3State() const { return QString::fromStdString(ds_.fx3_state); }
    int stage() const { return ds_.stage; }
    int generation() const { return static_cast<int>(ds_.generation); }
    double stateSeconds() const { return ds_.since_ns ? static_cast<double>(host_now_ns() - ds_.since_ns) * 1e-9 : 0; }
    int deviceProgress() const { return ds_.progress_pct; }
    bool warmingUp() const { return warming_; }
    QStringList startupReport() const { return report_; }
    double centerFreq() const { return cfg_.center_freq; }          // Source::config()(宣言。retune で即時更新)
    double appliedFreq() const { return applied_freq_; }             // Retune event の実周波数(適用済み)
    double sampleRate() const { return cfg_.sample_rate; }
    double gain() const { return cfg_.gain; }
    bool agc() const { return cfg_.agc; }
    QString antenna() const { return QString::fromStdString(cfg_.antenna); }
    bool sensorsValid() const { return ds_.sensors.valid; }
    bool loLocked() const { return ds_.sensors.lo_locked; }
    double rxTemp() const { return ds_.sensors.rx_temp_c; }
    double rssi() const { return ds_.sensors.rssi_db; }
    double driftPpm() const { return ds_.sensors.drift_ppm; }
    double driftUncertaintyPpm() const { return ds_.sensors.drift_uncertainty_ppm; }
    int overflowCount() const { return static_cast<int>(core_.events().count(EventKind::Overflow)); }
    int oosCount() const { return static_cast<int>(core_.events().count(EventKind::OutOfSequence)); }
    int timeoutCount() const { return static_cast<int>(core_.events().count(EventKind::Timeout)); }
    int discontinuityCount() const { return static_cast<int>(core_.events().count(EventKind::Discontinuity)); }
    int consumerOverflowCount() const { return static_cast<int>(core_.events().count(EventKind::ConsumerOverflow)); }
    QVariantList consumers() const { return consumers_; }
    double cpuMhz() const { return h_.cpu_mhz_avg; }
    QString governor() const { return QString::fromStdString(h_.governor); }
    double pkgTemp() const { return h_.pkg_temp_c; }
    int throttleCount() const { return static_cast<int>(h_.core_throttle_count + h_.pkg_throttle_count); }
    bool onAc() const { return h_.on_ac; }
    double diskFreeGb() const { return static_cast<double>(h_.disk_free_bytes) / 1e9; }
    double rssMb() const { return static_cast<double>(h_.rss_bytes) / 1e6; }
    double memAvailMb() const { return static_cast<double>(h_.mem_available_bytes) / 1e6; }
    int rtprioLimit() const { return static_cast<int>(h_.rtprio_limit); }
    double load1() const { return h_.load1; }
    QString activeApp() const { auto a = core_.active_app(); return a ? QString::fromStdString(a->name()) : QString(); }
    QString sourceName() const { return core_.has_source() ? QString::fromStdString(core_.source().name()) : QString(); }
    QString clock() const;
    int stateVersion() const { return static_cast<int>(state_version_); }
    EventListModel* events() { return &events_; }
    Q_INVOKABLE void refresh() { timerEvent(nullptr); }

    Q_INVOKABLE void retune(double hz);
    Q_INVOKABLE QString formatFreq(double hz) const;

Q_SIGNALS:
    void changed();
    void eventArrived(int severity, const QString& kind, const QString& detail);

protected:
    void timerEvent(QTimerEvent*) override;

private:
    Core& core_;
    DeviceStatus ds_;
    bool warming_ = false;
    QStringList report_;
    RfConfig cfg_;
    HealthSnapshot h_;
    QVariantList consumers_;
    double applied_freq_ = 0;   // Retune event(実際の周波数)
    uint64_t state_version_ = 0;
    EventListModel events_;
    int listener_ = 0;
    int timer_ = 0;
};

} // namespace spear::gui
