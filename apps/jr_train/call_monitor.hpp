// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 通話の自動選択。原本: crates/receiver-core/src/monitor.rs(commit dfc7440)。
//   各チャネルの「有効な音声フレームの時刻」(IQ の共通の時間軸、µs)から通話の始まりと終わりを推定し、始まりが早い順に並べる。
//   1 s 有効な音声フレームが無ければ通話の終わり。同期の確立に 3 フレーム(120 ms)かかるので、全チャネルがその時刻まで
//   処理を終えるまで始まりを決めない(スレッドの実行順で優先順位が変わらないように)。Reset(再同期・欠落)で通話を切る。
//   1 s 更新の無いチャネル(止まった worker)は他を待たせない。
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace spear::jr_train {

class CallMonitor {
public:
    using Clock = std::chrono::steady_clock;
    struct Activity {
        enum Kind { Voice, Reset } kind;
        uint64_t time_us;
    };
    explicit CallMonitor(std::vector<std::string> ids);
    void update(std::size_t channel, uint64_t progress_us, const std::vector<Activity>& activity);
    void finish();   // 入力の終わり(先読み待ちの始まりも確定させる)
    // (最優先の通話のチャネル, 通話中のチャネルを始まりの順に)
    std::pair<std::optional<std::string>, std::vector<std::string>> snapshot();
    // テスト用: 現在時刻を差し替える
    void set_clock(std::function<Clock::time_point()> now) { now_ = std::move(now); }
    void age(std::size_t channel, Clock::duration d) { tracks_.at(channel).updated -= d; }

private:
    struct Call {
        uint64_t start = 0, last = 0;
    };
    struct Track {
        std::string id;
        uint64_t progress = 0;
        Clock::time_point updated;
        std::deque<Activity> pending;
        std::optional<Call> call;
    };
    void advance();
    std::vector<Track> tracks_;
    bool finished_ = false;
    std::function<Clock::time_point()> now_ = [] { return Clock::now(); };
};

} // namespace spear::jr_train
