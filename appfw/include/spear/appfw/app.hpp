// S.P.E.A.R. appfw — App SDK (要件 §8, docs/app-development.md)
//
// App は「装置の 1 機能」。排他実行され、固定画面を持つ (§3.2, §9.3)。
// App が実装するもの:
//   * AppInfo: id / 名前 / 説明 / 方向
//   * RF 宣言: シェルの草案(draft)を set_rf_config() で受け取り、declare_rf_config() で Core へ返す
//   * start(Core&): Core の stream に subscribe して DSP チェーンを静的に構成する(UHD には触れない)
//   * stop(): 全 consumer を解放する
//   * QML ページ: apps/<id>/<Page>.qml。ページには `app`(この QObject)、`sys`(SystemModel)、`shell` が見える
//   * App 固有の State: この QObject の Q_PROPERTY として App 自身が所有する(docs/state-ownership.md)
//   * 再起動をまたいで残す State: コンストラクタで persist({"squelchDb", "view.dbMax", ...}) と宣言する。シェルの SettingsStore が
//     起動時に書き戻し、NOTIFY のたびに保存する(App に保存コードは要らない)。観測値・派生値・個体値(site.conf)は宣言しない。
// 登録は apps/CMakeLists.txt の spear_add_app() 1 行。レジストリはビルド時に生成される(§3.1 静的配線)。
#pragma once

#include "spear/core/app.hpp"
#include "spear/core/core.hpp"

#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace spear::appfw {

struct AppInfo {
    std::string id;            // "spectrum"(ディレクトリ名と一致)
    std::string name;          // 画面表示 "SPECTRUM"
    std::string description;   // 1 行
    Direction   direction = Direction::RX;
    std::string page_url;      // "qrc:/qt/qml/Spear/Apps/Spectrum/SpectrumPage.qml"
};

class GuiApp : public QObject, public spear::App {
    Q_OBJECT
    Q_PROPERTY(QString appId READ appId CONSTANT)
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
public:
    explicit GuiApp(AppInfo info, QObject* parent = nullptr) : QObject(parent), info_(std::move(info)) {}
    const AppInfo& info() const { return info_; }
    QString appId() const { return QString::fromStdString(info_.id); }
    QString appName() const { return QString::fromStdString(info_.name); }
    bool running() const { return running_; }

    // 起動時設定(録音先ディレクトリ等)。シェルが全 App に同じ map を渡す。必要なキーだけ読む。
    // 保存値の復元より後に呼ばれる(--set / site.conf が保存値より優先)。
    virtual void configure(const QVariantMap& /*settings*/) {}
    // 再起動をまたいで残すプロパティ(WRITE + NOTIFY を持つもの。"child.prop" は 1 段の子 QObject)
    const QStringList& persistedProperties() const { return persisted_; }

    // ---- spear::App ----
    std::string name() const override { return info_.name; }
    RfConfig declare_rf_config() const override { return rf_; }
    void set_rf_config(const RfConfig& c) { rf_ = c; }

    // 派生は on_start / on_stop を実装する(running フラグと signal は基底が扱う)。
    // SDK の保証: on_start / on_stop は必ずこの QObject のスレッド(GUI thread)で呼ばれる。
    // Core::run_app は装置の適用を待つため worker thread から呼ばれるが、ここで GUI thread へ渡す。
    // App 作者は QTimer / signal / QML property を普通に使ってよい。
    void start(Core& core) final;
    void stop() final;

Q_SIGNALS:
    void runningChanged();

protected:
    virtual void on_start(Core& core) = 0;
    virtual void on_stop() = 0;
    Core* core() const { return core_; }
    void persist(std::initializer_list<const char*> props) { for (const char* p : props) persisted_.push_back(QString::fromLatin1(p)); }

private:
    AppInfo info_;
    RfConfig rf_;
    Core* core_ = nullptr;
    bool running_ = false;
    QStringList persisted_;
};

inline void GuiApp::start(Core& core) {
    auto body = [this, &core] { core_ = &core; on_start(core); running_ = true; Q_EMIT runningChanged(); };
    if (QThread::currentThread() == thread()) body();
    else QMetaObject::invokeMethod(this, body, Qt::BlockingQueuedConnection);
}
inline void GuiApp::stop() {
    auto body = [this] { on_stop(); running_ = false; core_ = nullptr; Q_EMIT runningChanged(); };
    if (QThread::currentThread() == thread()) body();
    else QMetaObject::invokeMethod(this, body, Qt::BlockingQueuedConnection);
}

// ---- レジストリ(ビルド時生成。apps/CMakeLists.txt 参照) ----
struct AppEntry {
    AppInfo info;
    std::function<std::shared_ptr<GuiApp>()> make;
};
const std::vector<AppEntry>& registered_apps();

} // namespace spear::appfw
