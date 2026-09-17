// App: SPECTRUM — radio.rx の spectrum + waterfall。
// 検証対象(docs/apps/spectrum.md): App SDK の最小形、SpectrumView 部品、Core への retune。
#pragma once

#include "spear/appfw/app.hpp"
#include "spear/appfw/view_source.hpp"

#include <memory>

namespace spear::apps {

class SpectrumApp : public appfw::GuiApp {
    Q_OBJECT
    Q_PROPERTY(spear::appfw::ViewSource* view READ view CONSTANT)
public:
    explicit SpectrumApp(appfw::AppInfo info, QObject* parent = nullptr) : appfw::GuiApp(std::move(info), parent) {}
    appfw::ViewSource* view() { return &view_; }

    // 周波数は Core(Source)が所有する State。App は要求を渡すだけ。表示は sys.centerFreq。
    Q_INVOKABLE void tune(double hz) { if (core()) core()->source().retune(hz); }
    Q_INVOKABLE void stepFreq(double delta_hz) { if (core()) tune(core()->source().config().center_freq + delta_hz); }

protected:
    void on_start(Core& core) override;
    void on_stop() override;

    appfw::ViewSource view_;
    std::unique_ptr<appfw::ViewProcessor> vp_;
};

} // namespace spear::apps
