// S.P.E.A.R. core — App interface (要件 §8)
//
// ライフサイクル: create → declare_rf_config → start → (running) → stop → destroy
// App は Core が用意した Source(の output stream)に subscribe し、内部で DSP チェーンを静的に構成し、
// GUI へ渡す view frame を生成する。App は UHD へ直接アクセスしない (§2)。
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
};

} // namespace spear
