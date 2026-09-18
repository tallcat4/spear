#include "boot_sound.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace spear::gui {
namespace {

constexpr unsigned kRate = UiAudio::kRate;

// 起動音: 37% パルス波の単旋律 4 音(C3 G3 C4 C5 = ド ソ ド' ド'')。ブート完了時に 1 回。
// パルスは kBootMaxHarmonicHz までの倍音で作る(素のパルスは Nyquist まで倍音が伸びる。タップ音の 8 kHz より上まで入れて
// 少し明るく)。立ち上がりは直線 attack、音の途中で decay_to まで直線で下げ(オルガン的な平坦さを避ける)、直線 release で切る。
// 音同士は時間的に重ねない(重音・残響は雰囲気に合わなかった)。
constexpr double kBootPulseWidth = 0.37;
constexpr double kBootMaxHarmonicHz = 12000;

std::vector<float> pulse_note(double freq_hz, double length_ms, float amp,
                              double attack_ms = 5, double release_ms = 40, double decay_to = 0.7) {
    const auto n = static_cast<std::size_t>(kRate * length_ms / 1e3);
    const int harmonics = static_cast<int>(kBootMaxHarmonicHz / freq_hz);
    const double t_len = length_ms / 1e3, t_att = attack_ms / 1e3, t_rel = release_ms / 1e3;
    std::vector<float> s(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        double w = 0;
        for (int k = 1; k <= harmonics; ++k)
            w += 2.0 / (k * std::numbers::pi) * std::sin(k * std::numbers::pi * kBootPulseWidth)
                 * std::cos(2 * std::numbers::pi * k * freq_hz * t);
        double env;
        if (t < t_att)                 env = t / t_att;
        else if (t > t_len - t_rel)    env = decay_to * (t_len - t) / t_rel;
        else                           env = 1.0 - (1.0 - decay_to) * (t - t_att) / (t_len - t_att - t_rel);
        s[i] = static_cast<float>(amp * env * w);
    }
    return s;
}

// notes: {周波数 Hz, 開始 ms, 長さ ms}。開始位置に置くだけで加算はしない(重ならない前提)
struct BootNote { double freq_hz, start_ms, length_ms; };

std::vector<float> boot_melody(std::initializer_list<BootNote> notes, float amp, double tail_ms = 300) {
    double end_ms = 0;
    for (const auto& nt : notes) end_ms = std::max(end_ms, nt.start_ms + nt.length_ms);
    std::vector<float> s(static_cast<std::size_t>(kRate * (end_ms + tail_ms) / 1e3), 0.f);
    for (const auto& nt : notes) {
        const auto tone = pulse_note(nt.freq_hz, nt.length_ms, amp);
        std::copy(tone.begin(), tone.end(), s.begin() + static_cast<std::ptrdiff_t>(kRate * nt.start_ms / 1e3));
    }
    return s;
}

} // namespace

BootSound::BootSound(UiAudio& out, QObject* parent)
    : QObject(parent), out_(out),
      // ド ソ ド' ド''(C3 G3 C4 C5 = MIDI 48 55 60 72)、0.13 s 間隔で 3 音 80 ms、最終音 450 ms
      melody_(boot_melody({{130.81, 0, 80}, {196.00, 130, 80}, {261.63, 260, 80}, {523.25, 390, 450}}, 0.7f)) {}

void BootSound::play() { out_.play(melody_); }

} // namespace spear::gui
