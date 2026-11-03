// S.P.E.A.R. dsp — 音声のレベル合わせ(ゆっくりした AGC + 先読みのピークリミッタ)
//   どの App の音声も同じ大きさで AudioSink に渡し、OS の音量だけで全体の大きさが決まるようにする(docs/audio.md)。
//   音量の調整ではない(AudioSink と OS の音量の役割は変えない)。
//   * 数学的定義のみ(目標・上限・時定数は引数)。復調・復号の方式に依らない
//   * 音声を出す App がすべて使う(JR TRAIN / STD-T98 / SLPR / FM・AM)
//   * テスト: tests/test_leveler.cpp
// 段の規約: 入出力 rate は同じ、群遅延 = 先読みの sample 数(出力の長さは入力と同じで、その分遅れる)。
#pragma once

#include "stage.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace spear::dsp {

struct LevelerConfig {
    double target_dbfs = -18.0;    // 声のある所の rms の目標
    double ceiling_dbfs = -1.0;    // 出力の最大
    double gate_dbfs = -50.0;      // 10 ms の rms がこれより小さい所は無音とみなし、利得を変えない(雑音・無音を持ち上げない)
    double max_gain_db = 24.0;
    double min_gain_db = -12.0;
    double level_s = 1.5;          // 声のある所の平均電力を追う時定数(音節の抑揚には追従しない。大きすぎる出だしはリミッタが抑える)。
                                   // 話し始め(と new_talk_s 以上の無音のあと)はそこまでの累積平均で速く追う
    double new_talk_s = 2.0;
    double gain_s = 0.05;          // 利得の平滑
    double lookahead_s = 0.003;    // リミッタの先読み(= 群遅延)
    double release_s = 0.08;       // リミッタの戻り
};

class Leveler {
public:
    Leveler(std::string name, double rate, LevelerConfig cfg = {});
    StageInfo info() const { return info_; }
    void reset();
    // in-place。出力は lookahead_s だけ遅れる(長さは同じ)
    void process(std::span<float> io);
    double gain_db() const;        // 今の AGC の利得(リミッタを除く)

private:
    void block(float rms);
    StageInfo info_;
    LevelerConfig cfg_;
    double rate_;
    std::size_t block_n_, la_, new_talk_blocks_;
    double target_, ceiling_, gate_, max_gain_, min_gain_, a_level_, a_gain_, a_att_, a_rel_;
    // AGC
    double level_ = 0;             // 声のある所の平均電力
    std::size_t active_ = 0, silent_ = 0;   // 区切り以降の声のブロック数、続いている無音のブロック数
    double want_ = 1, gain_ = 1;
    double acc_ = 0;
    std::size_t acc_n_ = 0;
    // リミッタ
    std::vector<float> delay_;     // la_ sample の遅延
    std::vector<float> need_;      // 遅延と同じ並びの「その sample に要る利得」
    std::size_t pos_ = 0;
    double lim_ = 1;
};

} // namespace spear::dsp
