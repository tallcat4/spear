#include "tap_sound.hpp"

#include <cmath>
#include <numbers>

namespace spear::gui {
namespace {

constexpr unsigned kRate = 48000;
// ALSA バッファ。復調音声の 100 ms ではタップから発音までが遅れて感じる。詰めすぎると FZ-G2 の熱スロットリング時に underrun が出る
constexpr unsigned kLatencyUs = 50000;

// 減衰する周期波 1 発。計器らしい硬い音にするため波形は正弦ではなく鋸歯 / 矩形(倍音が立つ)。
// 鋸歯は kMaxHarmonicHz までの倍音だけで作る(帯域制限。素の鋸歯は Nyquist まで倍音が伸びて耳障りなヒスになる)。
// 立ち上がりは attack_ms の raised-cosine(急峻すぎると周波数領域で広がって不快な「パチッ」になる)。
// 音色・長さ・振幅を決める定数はコンストラクタの 2 行だけ(実機で耳で合わせる)。
// lead_ms の無音を先頭に置く(拒否音はタップ音の直後に同じ ring へ続くので、間を空けて 2 つの音に聞こえるように)
enum class Shape { Saw, Square };
constexpr double kMaxHarmonicHz = 8000;

std::vector<float> burst(Shape shape, double freq_hz, double length_ms, double attack_ms, double decay_ms, float amp, double lead_ms = 0) {
    const auto lead = static_cast<std::size_t>(kRate * lead_ms / 1e3);
    const auto n = static_cast<std::size_t>(kRate * length_ms / 1e3);
    const int harmonics = static_cast<int>(kMaxHarmonicHz / freq_hz);
    std::vector<float> s(lead + n, 0.f);
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
        s[lead + i] = static_cast<float>(amp * env * std::exp(-t / (decay_ms / 1e3)) * w);
    }
    return s;
}

} // namespace

TapSound::TapSound(EventBus* events, const std::string& device, QObject* parent)
    : QObject(parent), sink_(nullptr, device, kRate, kRate / 8, kLatencyUs),
      // タップ音: 帯域制限鋸歯 1 kHz、1.5 ms で立ち上がり 6 ms で減衰する「カチ」。拒否音: 矩形 300 Hz の短いブザー。
      // 振幅はエネルギーが揃うように決めている(タップ音は短いので peak は大きく、拒否音は長いので小さく。両方 ≈ −33 dB·s)
      tap_(burst(Shape::Saw, 1000, 20, 1.5, 6, 0.8f)), reject_(burst(Shape::Square, 300, 60, 0.1, 40, 0.18f, 15)) {
    std::string err;
    if (!sink_.open(&err) && events) events->emit(EventKind::Warning, "ui", "tap sound disabled: " + err);   // GUI は無音で続く
}

TapSound::~TapSound() = default;

void TapSound::tap() { sink_.write(tap_); }
void TapSound::reject() { sink_.write(reject_); }

} // namespace spear::gui
