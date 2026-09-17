// App テンプレート — 新しい App はこれをコピーして id / 名前 / DSP / ページを書き換える。
//
// 契約 (docs/app-development.md):
//   1. AppInfo は CMake の spear_add_app() から注入される(ここでは触らない)
//   2. on_start(Core&): Core の stream に subscribe し、DSP チェーンを静的に構成する。UHD には触れない
//   3. on_stop(): consumer と thread を全て解放する(次の App が Radio を使う)
//   4. App 固有の State は Q_PROPERTY として App が所有し、変更は App のメソッドだけを通る(state-ownership)
//   5. on_start / on_stop は GUI thread で呼ばれる(基底が保証)。DSP は自前 thread で回す
//   6. 中間 stream には TAP() を置く (§5.3)
#pragma once

#include "spear/appfw/app.hpp"
#include "spear/core/stream.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace spear::apps {

class TemplateApp final : public appfw::GuiApp {
    Q_OBJECT
    // 例: この App が所有する State。ページは読むだけ、変更は setThreshold() 経由。
    Q_PROPERTY(double threshold READ threshold WRITE setThreshold NOTIFY thresholdChanged)
    Q_PROPERTY(double blocksSeen READ blocksSeen NOTIFY statsChanged)
public:
    explicit TemplateApp(appfw::AppInfo info, QObject* parent = nullptr) : appfw::GuiApp(std::move(info), parent) {}
    double threshold() const { return threshold_; }
    void setThreshold(double v) { threshold_ = v; Q_EMIT thresholdChanged(); }
    double blocksSeen() const { return static_cast<double>(blocks_.load()); }

Q_SIGNALS:
    void thresholdChanged();
    void statsChanged();

protected:
    void on_start(Core& core) override;
    void on_stop() override;
    void timerEvent(QTimerEvent*) override { Q_EMIT statsChanged(); }

private:
    void run();
    std::shared_ptr<Subscription> sub_;   // Core の stream への購読(Lossless か LatestOnly を選ぶ)
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> blocks_{0};
    double threshold_ = -60.0;
    int timer_ = 0;
};

} // namespace spear::apps
