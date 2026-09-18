#include "tap_sound.hpp"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spear::gui {
namespace {

constexpr unsigned kRate = UiAudio::kRate;

// 減衰する周期波 1 発。計器らしい硬い音にするため波形は正弦ではなく鋸歯 / 矩形(倍音が立つ)。
// 鋸歯は kMaxHarmonicHz までの倍音だけで作る(帯域制限。素の鋸歯は Nyquist まで倍音が伸びて耳障りなヒスになる)。
// 立ち上がりは attack_ms の raised-cosine(急峻すぎると周波数領域で広がって不快な「パチッ」になる)。
// 音色・長さ・振幅を決める定数はコンストラクタの 2 行だけ(実機で耳で合わせる)。
enum class Shape { Saw, Square };
constexpr double kMaxHarmonicHz = 8000;

std::vector<float> burst(Shape shape, double freq_hz, double length_ms, double attack_ms, double decay_ms, float amp) {
    const auto n = static_cast<std::size_t>(kRate * length_ms / 1e3);
    const int harmonics = static_cast<int>(kMaxHarmonicHz / freq_hz);
    std::vector<float> s(n, 0.f);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        double w = 0;
        if (shape == Shape::Saw) {
            for (int k = 1; k <= harmonics; ++k) w += (k % 2 ? 1.0 : -1.0) * std::sin(2 * std::numbers::pi * k * freq_hz * t) / k;
            w *= 2 / std::numbers::pi;
        } else {
            w = std::fmod(freq_hz * t, 1.0) < 0.5 ? 1.0 : -1.0;
        }
        const double a = t / (attack_ms / 1e3);
        const double env = a < 1 ? 0.5 - 0.5 * std::cos(std::numbers::pi * a) : 1.0;
        s[i] = static_cast<float>(amp * env * std::exp(-t / (decay_ms / 1e3)) * w);
    }
    return s;
}

} // namespace

TapSound::TapSound(UiAudio& out, QObject* parent)
    : QObject(parent), out_(out),
      // タップ音: 帯域制限鋸歯 1 kHz、1.5 ms で立ち上がり 6 ms で減衰する「カチ」。拒否音: 矩形 300 Hz の短いブザー。
      // 振幅はエネルギーが揃うように決めている(タップ音は短いので peak は大きく、拒否音は長いので小さく。両方 ≈ −33 dB·s)
      tap_(burst(Shape::Saw, 1000, 20, 1.5, 6, 0.8f)), reject_(burst(Shape::Square, 300, 60, 0.1, 40, 0.18f)) {}

void TapSound::tap() { tap_pending_ = true; flush(); }
void TapSound::reject() { reject_pending_ = true; flush(); }

void TapSound::flush() {
    if (flush_scheduled_) return;
    flush_scheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        flush_scheduled_ = false;
        if (reject_pending_) out_.play(reject_);
        else if (tap_pending_) out_.play(tap_);
        tap_pending_ = reject_pending_ = false;
    });
}

} // namespace spear::gui
