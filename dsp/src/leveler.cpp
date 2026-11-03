#include "spear/dsp/leveler.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace spear::dsp {

namespace {
double db_to_lin(double db) { return std::pow(10.0, db / 20.0); }
// 時定数 tau の一次 IIR の係数(dt ごとに 1 回更新)
double coeff(double dt, double tau) { return tau > 0 ? 1.0 - std::exp(-dt / tau) : 1.0; }
} // namespace

Leveler::Leveler(std::string name, double rate, LevelerConfig cfg) : cfg_(cfg), rate_(rate) {
    if (!(rate > 0)) throw std::invalid_argument("Leveler: rate must be positive");
    block_n_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(rate * 0.01)));
    la_ = static_cast<std::size_t>(std::lround(std::max(0.0, cfg.lookahead_s) * rate));
    target_ = db_to_lin(cfg.target_dbfs);
    ceiling_ = db_to_lin(cfg.ceiling_dbfs);
    gate_ = db_to_lin(cfg.gate_dbfs);
    max_gain_ = db_to_lin(cfg.max_gain_db);
    min_gain_ = db_to_lin(cfg.min_gain_db);
    const double block_s = static_cast<double>(block_n_) / rate;
    a_level_ = coeff(block_s, cfg.level_s);
    new_talk_blocks_ = static_cast<std::size_t>(std::lround(cfg.new_talk_s / block_s));
    a_gain_ = coeff(1.0 / rate, cfg.gain_s);
    a_att_ = la_ > 0 ? coeff(1.0, static_cast<double>(la_) / 4.0) : 1.0;   // 先読みの間にほぼ(e^-4)下げ切る
    a_rel_ = coeff(1.0 / rate, cfg.release_s);
    info_.name = std::move(name);
    info_.in_rate = rate;
    info_.out_rate = rate;
    info_.group_delay_in = static_cast<double>(la_);
    reset();
}

void Leveler::reset() {
    level_ = 0;
    active_ = silent_ = 0;
    want_ = gain_ = 1;
    acc_ = 0;
    acc_n_ = 0;
    delay_.assign(la_, 0.f);
    need_.assign(la_ + 1, 1.f);
    pos_ = 0;
    lim_ = 1;
}

double Leveler::gain_db() const { return 20.0 * std::log10(gain_); }

// 10 ms ごと: 声のある所だけ平均電力を追い、利得の目標を決める
void Leveler::block(float rms) {
    if (rms <= gate_) {
        if (++silent_ >= new_talk_blocks_) active_ = 0;
        return;
    }
    silent_ = 0;
    const double p = static_cast<double>(rms) * rms;
    ++active_;
    level_ += (p - level_) * std::max(a_level_, 1.0 / static_cast<double>(active_));   // 最初は累積平均
    want_ = std::clamp(target_ / std::sqrt(level_), min_gain_, max_gain_);
}

void Leveler::process(std::span<float> io) {
    const std::size_t window = la_ + 1;
    for (float& s : io) {
        const double x = s;
        acc_ += x * x;
        if (++acc_n_ == block_n_) {
            block(static_cast<float>(std::sqrt(acc_ / static_cast<double>(block_n_))));
            acc_ = 0;
            acc_n_ = 0;
        }
        gain_ += (want_ - gain_) * a_gain_;
        const double y = x * gain_;
        // この sample に要る利得。先読みの窓(これから出る la_ + 1 sample)の最小に、利得をなめらかに寄せる
        const double ay = std::abs(y);
        need_[pos_ % window] = static_cast<float>(ay > ceiling_ ? ceiling_ / ay : 1.0);
        const double m = *std::min_element(need_.begin(), need_.end());
        lim_ += (m - lim_) * (m < lim_ ? a_att_ : a_rel_);
        double out = y;
        if (la_ > 0) {
            out = delay_[pos_ % la_];
            delay_[pos_ % la_] = static_cast<float>(y);
        }
        s = static_cast<float>(std::clamp(out * lim_, -ceiling_, ceiling_));
        ++pos_;
    }
}

} // namespace spear::dsp
