#include "spectrum_app.hpp"

namespace spear::apps {

void SpectrumApp::on_start(Core& core) {
    vp_ = std::make_unique<appfw::ViewProcessor>(core.rx(), info().id + ".view");
    view_.setProcessor(vp_.get());
    vp_->start();
}

void SpectrumApp::on_stop() {
    view_.setProcessor(nullptr);
    vp_.reset();
}

} // namespace spear::apps
