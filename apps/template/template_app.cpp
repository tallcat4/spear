#include "template_app.hpp"
#include "spear/core/tap.hpp"

namespace spear::apps {

using namespace std::chrono_literals;

void TemplateApp::on_start(Core& core) {
    // decoder のように全 sample が要るなら Lossless、表示だけなら LatestOnly (§10)
    sub_ = core.rx().subscribe(info().id, DeliveryPolicy::Lossless, 64);
    stop_ = false;
    th_ = std::thread([this] { run(); });
    timer_ = startTimer(250);   // GUI thread で呼ばれているので QObject の timer が使える
}

void TemplateApp::on_stop() {
    if (timer_) { killTimer(timer_); timer_ = 0; }
    stop_ = true;
    if (th_.joinable()) th_.join();
    sub_.reset();
}

void TemplateApp::run() {
    while (!stop_) {
        auto d = sub_->pop(100ms);
        if (!d) continue;
        auto iq = d->block.as<sc16>();
        // ここに DSP。中間結果は TAP() で観測できるようにする (§5.3)
        TAP(std::string(info().id + ".input").c_str(), iq.data(), iq.size());
        blocks_++;
        // 事象を見つけたら Event に provenance(元 sample index 範囲)を付けて発行する (§4.5)
        // core()->events().emit(EventKind::Info, info().id, "something", d->block.range());
    }
}

} // namespace spear::apps
