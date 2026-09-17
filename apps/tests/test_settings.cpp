// SettingsStore(運転状態の自動保存・復元)の検証。
//   * 型ごとの往復(bool / int / double / QString、子 QObject のドット記法)
//   * 復元は保存を起こさない、宣言の誤り(存在しないプロパティ)は数えられる
//   * 登録済みの全 App の persist 宣言が実在のプロパティに解決すること(名前の打ち間違いをビルド後すぐ捕まえる)
//   * ViewSource の手動レンジが processor の付け替えをまたいで残ること
#include "spear/appfw/app.hpp"
#include "spear/appfw/settings_store.hpp"
#include "spear/appfw/view_source.hpp"
#include "spear/core/core.hpp"
#include "spear/core/synthetic_source.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace spear;

namespace {
class Child : public QObject {
    Q_OBJECT
    Q_PROPERTY(double level READ level WRITE setLevel NOTIFY changed)
public:
    double level() const { return level_; }
    void setLevel(double v) { level_ = v; Q_EMIT changed(); }
Q_SIGNALS:
    void changed();
private:
    double level_ = -60;
};

class Probe : public QObject {
    Q_OBJECT
    Q_PROPERTY(double squelch READ squelch WRITE setSquelch NOTIFY configChanged)
    Q_PROPERTY(int channel READ channel WRITE setChannel NOTIFY configChanged)   // 同じ NOTIFY を共有
    Q_PROPERTY(bool mute READ mute WRITE setMute NOTIFY muteChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QObject* view READ view CONSTANT)
    Q_PROPERTY(double readOnly READ readOnly NOTIFY configChanged)
public:
    double squelch() const { return squelch_; }
    void setSquelch(double v) { squelch_ = v; Q_EMIT configChanged(); }
    int channel() const { return channel_; }
    void setChannel(int v) { channel_ = v; Q_EMIT configChanged(); }
    bool mute() const { return mute_; }
    void setMute(bool v) { mute_ = v; Q_EMIT muteChanged(); }
    QString mode() const { return mode_; }
    void setMode(const QString& m) { mode_ = m; Q_EMIT modeChanged(); }
    QObject* view() { return &view_; }
    double readOnly() const { return 1; }
Q_SIGNALS:
    void configChanged();
    void muteChanged();
    void modeChanged();
private:
    double squelch_ = -40;
    int channel_ = 0;
    bool mute_ = false;
    QString mode_ = "NFM";
    Child view_;
};

const QStringList kProbeProps = {"squelch", "channel", "mute", "mode", "view.level"};
} // namespace

TEST(Settings, RoundTripThroughFile) {
    QTemporaryDir dir;
    const QString path = dir.filePath("state.conf");
    {
        appfw::SettingsStore store(path);
        ASSERT_TRUE(store.load());
        Probe p;
        store.bind(&p, "probe", kProbeProps);
        EXPECT_EQ(store.boundCount(), 5);
        EXPECT_EQ(store.restoredCount(), 0);
        p.setSquelch(-52.5);
        p.setChannel(17);
        p.setMute(true);
        p.setMode("WFM");
        static_cast<Child*>(p.view())->setLevel(-35);
        store.flush();
    }
    ASSERT_TRUE(QFile::exists(path));
    {
        appfw::SettingsStore store(path);
        ASSERT_TRUE(store.load());
        Probe p;
        store.bind(&p, "probe", kProbeProps);
        EXPECT_EQ(store.restoredCount(), 5);
        EXPECT_DOUBLE_EQ(p.squelch(), -52.5);
        EXPECT_EQ(p.channel(), 17);
        EXPECT_TRUE(p.mute());
        EXPECT_EQ(p.mode(), "WFM");
        EXPECT_DOUBLE_EQ(static_cast<Child*>(p.view())->level(), -35);
        // 復元は保存を起こさない(値が変わっていないのでファイルは同じ)
        const auto before = store.values();
        store.flush();
        EXPECT_EQ(store.values(), before);
    }
}

TEST(Settings, UnresolvedDeclarationsAreCountedNotFatal) {
    appfw::SettingsStore store{QString()};
    Probe p;
    store.bind(&p, "probe", {"squelch", "noSuchProperty", "view.nope", "readOnly", "nochild.x"});
    EXPECT_EQ(store.boundCount(), 1);
    EXPECT_EQ(store.unresolvedCount(), 4);
}

TEST(Settings, EmptyPathKeepsValuesInMemoryOnly) {
    appfw::SettingsStore store{QString()};
    Probe p;
    store.bind(&p, "probe", kProbeProps);
    p.setSquelch(-70);
    store.flush();
    EXPECT_EQ(store.values()["probe.squelch"].toString(), "-70");
    EXPECT_TRUE(store.path().isEmpty());
}

TEST(Settings, EveryRegisteredAppDeclaresOnlyRealProperties) {
    appfw::SettingsStore store{QString()};
    for (const auto& e : appfw::registered_apps()) {
        auto app = e.make();
        const int before = store.unresolvedCount();
        store.bind(app.get(), app->appId(), app->persistedProperties());
        EXPECT_EQ(store.unresolvedCount(), before) << e.info.id << ": " << app->persistedProperties().join(", ").toStdString();
        EXPECT_FALSE(app->persistedProperties().isEmpty()) << e.info.id << " persists nothing (recorder is allowed)" << (e.info.id == "recorder" ? "" : "!");
    }
}

TEST(Settings, ViewSourceManualRangeSurvivesProcessorSwap) {
    Core core(".");
    SyntheticSignal sig; sig.tones = {{100e3, 0.3}};
    core.set_source(std::make_unique<SyntheticSource>(&core.events(), sig));
    appfw::ViewSource vs;
    // processor 無し = 起動時の復元と同じ経路: 値は覚えるが手動/自動は manualRange が決める(宣言順に依らない)
    vs.setDbMax(-20);
    vs.setDbMin(-110);
    vs.setAveraging(8);
    EXPECT_FALSE(vs.manualRange());
    EXPECT_DOUBLE_EQ(vs.dbMax(), -20);
    vs.setManualRange(true);
    appfw::ViewProcessor vp(core.rx(), "test.view");
    vs.setProcessor(&vp);
    float lo, hi;
    vp.get_db_range(lo, hi);
    EXPECT_FLOAT_EQ(lo, -110);
    EXPECT_FLOAT_EQ(hi, -20);
    // 表示中の REF 操作は手動に切り替える
    vs.setProcessor(nullptr);
    vs.autoRange();
    vs.setProcessor(&vp);
    vs.setDbMax(-30);
    EXPECT_TRUE(vs.manualRange());
    vs.setProcessor(nullptr);
    // 自動に戻すと次の processor では自動レンジ(既定のまま)
    vs.autoRange();
    EXPECT_FALSE(vs.manualRange());
    appfw::ViewProcessor vp2(core.rx(), "test.view2");
    vs.setProcessor(&vp2);
    EXPECT_EQ(vs.averaging(), 8);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "test_settings.moc"
