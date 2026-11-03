// S.P.E.A.R. core — App interface (要件 §7)
//
// ライフサイクル: create → declare_rf_config → start → (running) → stop → destroy
// 運転中の RF の構成し直し(Core::reconfigure): rf_will_change → (Core が Source を構成し直す) → rf_changed。
// App は止まらない(App 固有の State・画面はそのまま)。Source の stream は作り直されるので、App は購読・送信の入口を付け直す。
// App は Core が用意した Source(の output stream)に subscribe し、内部で DSP チェーンを静的に構成し、
// GUI へ渡す view frame を生成する。App は UHD へ直接アクセスしない (§2.1)。
#pragma once

#include "types.hpp"

#include <string>

namespace spear {

class Core;

class App {
public:
    virtual ~App() = default;
    virtual std::string name() const = 0;
    virtual RfConfig declare_rf_config() const = 0;
    virtual void start(Core& core) = 0;   // Core はこの時点で RF 条件を適用済み
    virtual void stop() = 0;
    // Core::reconfigure の前後。Source はもう止まっている(rf_will_change)/ まだ始まっていない(rf_changed)。
    // 既定は stop / start(stream を付け直すだけの App はこれで足りる)
    virtual void rf_will_change() { stop(); }
    virtual void rf_changed(Core& core) { start(core); }
};

} // namespace spear
