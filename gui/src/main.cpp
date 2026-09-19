// spear-gui — エントリ。Core + Source を組み、QML シェルを起動する (要件 §9)
//
//   spear-gui [--source b210|synthetic|file:<base>] [--rate 4e6] [--freq 100e6] [--gain 30]   (既定: b210)
//             [--record-dir ./recordings] [--event-log path] [--state-file path] [--screenshot out.png --after 5]
//             [--fullscreen] [--width W --height H]
// --state-file: 運転状態(ゲイン/AGC、各 App のスケルチ・MUTE・表示レンジ…)の自動保存先。無ければ保存も復元もしない(検証用の起動)。
#include "shell.hpp"
#include "system_model.hpp"
#include "boot_sound.hpp"
#include "tap_sound.hpp"
#include "ui_audio.hpp"
#include "spear/appfw/settings_store.hpp"
#include "spear/core/event_log.hpp"
#include "spear/core/recording.hpp"
#include "spear/core/synthetic_source.hpp"
#if SPEAR_WITH_UHD
#include "spear/core/b210_source.hpp"
#endif

#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace spear;
using namespace spear::gui;

namespace {
const char* arg(int argc, char** argv, const char* key, const char* def) {
    for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return def;
}
bool flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], key)) return true;
    return false;
}
} // namespace

int main(int argc, char** argv) {
    // 専用機の固定パネル (1920x1200) 前提: デスクトップの HiDPI 倍率を無視し、物理 px でレイアウトする (§1)
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", "1");
    QGuiApplication app(argc, argv);
    app.setApplicationName("spear");

    // 既定は実機。合成データは明示的に --source synthetic を指定したときだけ(専用機の既定動作は実機)
#if SPEAR_WITH_UHD
    const std::string source = arg(argc, argv, "--source", "b210");
#else
    const std::string source = arg(argc, argv, "--source", "synthetic");
#endif
    RfConfig cfg;
    cfg.sample_rate = std::stod(arg(argc, argv, "--rate", "4e6"));
    cfg.center_freq = std::stod(arg(argc, argv, "--freq", "100e6"));
    cfg.gain        = std::stod(arg(argc, argv, "--gain", "30"));
    cfg.agc         = flag(argc, argv, "--agc");
    cfg.antenna     = arg(argc, argv, "--ant", "RX2");
    const std::string record_dir = arg(argc, argv, "--record-dir", "recordings");
    // --set key=value(複数可): 個体・現場固有の設定。spear.sh が ~/spear/site.conf から渡す。
    // 装置の値(radio.freq_err_ppm)は Source の生成に要るのでここで読む。App 固有(<id>.<name>)と audio_device は App / シェルへ配る
    QVariantMap settings;
    settings["record_dir"] = QString::fromStdString(record_dir);
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--set") != 0) continue;
        const QString kv = QString::fromUtf8(argv[++i]);
        const qsizetype eq = kv.indexOf('=');
        if (eq > 0) settings[kv.left(eq)] = kv.mid(eq + 1);
    }

    Core core(".");
    std::unique_ptr<EventLogFile> evlog;
    if (const char* p = arg(argc, argv, "--event-log", nullptr)) evlog = std::make_unique<EventLogFile>(core.events(), p);

    std::unique_ptr<Source> src;
    if (source == "synthetic") {
        SyntheticSignal sig;
        sig.tones = {{cfg.sample_rate * 0.1, 0.3}, {-cfg.sample_rate * 0.23, 0.05}, {cfg.sample_rate * 0.31, 0.01}};
        sig.noise_amplitude = 0.004;
        src = std::make_unique<SyntheticSource>(&core.events(), sig);
    } else if (source.rfind("file:", 0) == 0) {
        auto rs = std::make_unique<RecordingSource>(&core.events(), source.substr(5), true);
        std::string err;
        if (!rs->open(&err)) { std::fprintf(stderr, "open recording: %s\n", err.c_str()); return 1; }
        cfg = rs->config();
        src = std::move(rs);
    }
#if SPEAR_WITH_UHD
    else if (source == "b210") {
        B210SourceOptions opt;
        opt.radio.expected_serial = arg(argc, argv, "--serial", "");
        opt.radio.device_args = arg(argc, argv, "--args", "");
        opt.radio.freq_err_ppm = settings.value("radio.freq_err_ppm", 0.0).toDouble();   // 個体の LO 誤差(site.conf)。Radio が LO 側で打ち消す
        src = std::make_unique<B210LiveSource>(&core.events(), opt);
    }
#endif
    else { std::fprintf(stderr, "unknown source %s\n", source.c_str()); return 1; }
    core.set_source(std::move(src));

    SystemModel model(core);
    Shell shell(core, cfg);
    // 運転状態の復元 → 起動時設定(--set)の順。保存値はコマンドラインの既定(--freq/--rate/--gain)より優先、--set(site.conf)はさらに優先
    appfw::SettingsStore store(QString::fromUtf8(arg(argc, argv, "--state-file", "")));
    {
        QString err;
        if (!store.load(&err)) std::fprintf(stderr, "state: %s\n", qPrintable(err));
        // シェルの草案: ゲイン/AGC はどの Source でも(setDraftGain は AGC を切るので AGC を後に復元)。周波数/レートは録音再生では録音条件が真値なので残さない
        QStringList shell_props = {"draftGain", "draftAgc"};
        if (source.rfind("file:", 0) != 0) shell_props << "draftFreq" << "draftRate";
        store.bind(&shell, "shell", shell_props);
        for (const auto& app : shell.instances()) store.bind(app.get(), app->appId(), app->persistedProperties());
        if (!store.path().isEmpty()) std::fprintf(stderr, "state: %s (%d restored, %d bound)\n", qPrintable(store.path()), store.restoredCount(), store.boundCount());
    }
    core.source().configure(shell.draft());   // 起動時点の宣言を適用(App 起動前でも status bar に正しい RF を出す)
    core.source().warm_up();                  // 実機: 自己診断 → 装置待ち → FPGA ロード → tune 確認 → close(スプラッシュが経過を見せる)
    for (const auto& app : shell.instances()) app->configure(settings);   // 保存値の復元より後(site.conf が勝つ)
    // GUI の音(操作音・起動音)。デバイスは App 音声と同じ audio_device 設定(--set audio_device=null で無音にできる)
    UiAudio ui_audio(&core.events(), settings.value("audio_device", "default").toString().toStdString());
    TapSound tap_sound(ui_audio);
    BootSound boot_sound(ui_audio);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("sys", &model);
    engine.rootContext()->setContextProperty("shell", &shell);
    engine.rootContext()->setContextProperty("tapSound", &tap_sound);
    engine.rootContext()->setContextProperty("bootSound", &boot_sound);
    engine.rootContext()->setContextProperty("startFullscreen", flag(argc, argv, "--fullscreen"));
    engine.rootContext()->setContextProperty("startWidth", std::stoi(arg(argc, argv, "--width", "1920")));
    engine.rootContext()->setContextProperty("startHeight", std::stoi(arg(argc, argv, "--height", "1200")));
    engine.loadFromModule("Spear", "Main");
    if (engine.rootObjects().isEmpty()) return 2;

    // 検証用: N 秒後にウィンドウを PNG に保存して終了
    if (const char* shot = arg(argc, argv, "--screenshot", nullptr)) {
        const int after = std::stoi(arg(argc, argv, "--after", "5"));
        const QString path = QString::fromUtf8(shot);
        const int start_app = std::stoi(arg(argc, argv, "--start-app", "-1"));
        // --start-app は立ち上げ(warm_up: 実機なら FPGA ロード〜tune 確認)が終わってから。--after はそれより長く取る
        if (start_app >= 0) {
            auto* t = new QTimer(&app);
            QObject::connect(t, &QTimer::timeout, [&core, &shell, start_app, t] {
                if (core.source().warming_up()) return;
                t->stop(); shell.startApp(start_app);
            });
            t->start(200);
        }
        if (flag(argc, argv, "--open-entry")) QTimer::singleShot(std::max(1000, after * 1000 - 1500), [&engine, argc, argv] {
            if (auto* win = engine.rootObjects().first()) {
                if (const char* mhz = arg(argc, argv, "--entry-tune", nullptr)) {
                    QMetaObject::invokeMethod(win, "demoEntryTune", Q_ARG(QVariant, QVariant(QString::fromUtf8(mhz))));
                    return;
                }
                const char* fn = flag(argc, argv, "--entry-error") ? "demoEntryError"
                               : flag(argc, argv, "--rapid-tune") ? "demoRapidTune" : "openFreqEntry";
                if (!QMetaObject::invokeMethod(win, fn)) std::fprintf(stderr, "%s: invoke failed\n", fn);
            }
        });
        if (const char* rr = arg(argc, argv, "--restart-rate", nullptr)) QTimer::singleShot(std::max(1000, after * 1000 - 3000), [&shell, rr] { shell.restartActiveWithRate(std::stod(rr)); });
        // --invoke <fn>: Main.qml の関数を撮影の 1.5 s 前に呼ぶ(例: openExitDialog)
        if (const char* fn = arg(argc, argv, "--invoke", nullptr)) QTimer::singleShot(std::max(1000, after * 1000 - 1500), [&engine, fn] {
            if (auto* win = engine.rootObjects().first())
                if (!QMetaObject::invokeMethod(win, fn)) std::fprintf(stderr, "%s: invoke failed\n", fn);
        });
        if (flag(argc, argv, "--diag")) QTimer::singleShot(std::max(1000, after * 1000 - 1500), [&shell] { shell.showDiagnostics(true); });
        if (const char* act = arg(argc, argv, "--app-action", nullptr)) QTimer::singleShot(std::max(1000, after * 1000 - 2500), [&engine, act] {
            if (auto* win = engine.rootObjects().first())
                QMetaObject::invokeMethod(win, "demoAppAction", Q_ARG(QVariant, QVariant(QString::fromUtf8(act))));
        });
        QTimer::singleShot(after * 1000, [&engine, path, &app, &shell] {
            auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (win) win->grabWindow().save(path);
            shell.stopApp();
            QTimer::singleShot(500, &app, &QGuiApplication::quit);
        });
    }
    const int rc = app.exec();
    core.stop_app();
    store.flush();
    return rc;
}
