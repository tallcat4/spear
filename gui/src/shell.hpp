// spear-gui — Shell: 起動 → App 一覧 → 選択 → 実行 → 戻る (要件 §9.1)
//
// App はレジストリ(ビルド時生成)から取る。active な App は常に 1 つ (§3.2)。
// RF 宣言の草案(draft*)はシェルが持ち、App 起動時に App へ渡して Core に宣言される。
// 運転中の RF 状態は Core が所有する(sys.* から読む。docs/state-ownership.md)。
#pragma once

#include "spear/appfw/app.hpp"
#include "spear/core/core.hpp"

#include <QObject>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <thread>
#include <vector>

namespace spear::gui {

class Shell : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("created by the application")
    Q_PROPERTY(QVariantList apps READ apps CONSTANT)              // メニュー項目(App + 末尾に DIAGNOSTICS)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY activeChanged)
    Q_PROPERTY(QObject* activeApp READ activeApp NOTIFY activeChanged)
    Q_PROPERTY(QString activePage READ activePage NOTIFY activeChanged)
    Q_PROPERTY(bool diagnosticsOpen READ diagnosticsOpen NOTIFY activeChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY activeChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY activeChanged)
    Q_PROPERTY(double draftFreq READ draftFreq WRITE setDraftFreq NOTIFY draftChanged)
    Q_PROPERTY(double draftRate READ draftRate WRITE setDraftRate NOTIFY draftChanged)
    Q_PROPERTY(double draftGain READ draftGain WRITE setDraftGain NOTIFY draftChanged)
    Q_PROPERTY(bool draftAgc READ draftAgc WRITE setDraftAgc NOTIFY draftChanged)
public:
    Shell(Core& core, RfConfig draft, QObject* parent = nullptr);
    ~Shell() override;

    QVariantList apps() const { return apps_; }
    int activeIndex() const { return active_; }
    QObject* activeApp() const { return active_ >= 0 && active_ < static_cast<int>(instances_.size()) ? instances_[active_].get() : nullptr; }
    QString activePage() const;
    bool diagnosticsOpen() const { return diagnostics_; }
    bool busy() const { return busy_; }
    QString lastError() const { return last_error_; }
    int diagnosticsIndex() const { return static_cast<int>(instances_.size()); }

    double draftFreq() const { return draft_.center_freq; }
    double draftRate() const { return draft_.sample_rate; }
    double draftGain() const { return draft_.gain; }
    void setDraftFreq(double v) { draft_.center_freq = v; Q_EMIT draftChanged(); }
    void setDraftRate(double v) { draft_.sample_rate = v; Q_EMIT draftChanged(); }
    void setDraftGain(double v) { draft_.gain = v; draft_.agc = false; Q_EMIT draftChanged(); }   // 数値を入れたら AGC は切れる
    bool draftAgc() const { return draft_.agc; }
    void setDraftAgc(bool on) { draft_.agc = on; Q_EMIT draftChanged(); }
    const RfConfig& draft() const { return draft_; }   // 復元後の草案を起動時の宣言に使う(main)

    Q_INVOKABLE void startApp(int index);   // 非同期。結果は activeChanged
    Q_INVOKABLE void stopApp();
    // 汎用 App(SPECTRUM / RECORDER)がサンプルレートを変えるとき: rate は運転中に変えられないので App を止めて同じ App を再起動する。
    // 目的が確定した App(STD-T98 等)は rate を自分で宣言するので使わない。
    Q_INVOKABLE void restartActiveWithRate(double sample_rate);
    Q_INVOKABLE void showDiagnostics(bool on) { diagnostics_ = on; Q_EMIT activeChanged(); }
    Q_INVOKABLE void skipWarmUp() { core_.source().cancel_warm_up(); }   // スプラッシュの CONTINUE(装置の出現待ちを打ち切る)
    // メニューの EXIT(確認後)。イベントループを抜け、main が App 停止と運転状態の保存をして終わる。
    // 非キオスクのデスクトップ環境でフルスクリーン起動している現状のための導線(Main.qml のコメント参照)。
    Q_INVOKABLE void quit();

    // 各 App インスタンス(起動前の設定注入などに使う)
    const std::vector<std::shared_ptr<appfw::GuiApp>>& instances() const { return instances_; }

Q_SIGNALS:
    void activeChanged();
    void draftChanged();

private:
    Core& core_;
    RfConfig draft_;
    QVariantList apps_;
    std::vector<std::shared_ptr<appfw::GuiApp>> instances_;
    int restart_index_ = -1;   // stopApp 完了後に再起動する App
    double pending_rate_ = 0;
    int active_ = -1;
    bool diagnostics_ = false;
    bool busy_ = false;
    QString last_error_;
    std::thread worker_;
};

} // namespace spear::gui
