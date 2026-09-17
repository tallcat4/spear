// チャネルごとの秘話セッション管理 — std_t98_multi_audio_service.py の SecretChannelState / _maybe_send_secret_request の移植。
//
// 秘話フレーム(SACCH call_stat = 1)が続く間を 1 セッションとし、FEC 後のバーストを窓に溜めて鍵探索の要求を出す:
//   * 窓が min_window(5)に達したら最初の要求(直近 5 バースト)
//   * 以後は recheck_interval(10)バーストごと、窓が max_window(10)に満ちているときに再確認(直近 10 バースト)
//   * 要求は 1 チャネルに同時 1 つ。結果はセッション番号が一致するときだけ採用し、鍵 > 0 なら現在鍵を更新
// 現在鍵はセッションをまたいで保持する(同じ相手はたいてい同じ鍵。次の呼でまず「今の鍵」を検証する)。
// 純ロジック(スレッド・時刻・I/O なし)。App が DSP thread 上で 1 チャネル 1 インスタンス持つ。
#pragma once
#include "pn.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace spear::std_t98::secret {

enum class TrackerStatus : uint8_t { Idle, Collecting, Pending, Keyed, Miss };
const char* to_string(TrackerStatus s);

struct TrackerConfig {
    int min_window = 5;
    int max_window = 10;
    int recheck_interval = 10;
};

class Tracker {
public:
    using Config = TrackerConfig;
    struct Request {
        uint32_t session = 0;
        uint16_t current_key = 0;
        std::vector<Burst> bursts;
    };
    Tracker(Config cfg = Config()) : cfg_(cfg) {}   // NOLINT: SecretChan{} の集約初期化で使う

    // 秘話 TCH フレームを 1 つ受け取った。要求を出すべきなら true と request を返す(呼び出し側がワーカーへ渡す)
    bool on_secret_burst(const Burst& b, Request* request);
    // 平文フレーム / 同期バースト / squelch 閉 → セッション終了(鍵は保持)
    void on_clear();
    // 探索結果。セッションが違えば無視
    void on_result(uint32_t session, uint16_t key);

    uint32_t session() const { return session_; }
    bool active() const { return active_; }
    bool pending() const { return pending_; }
    uint16_t key() const { return key_; }
    void set_key(uint16_t k) { key_ = k; }
    TrackerStatus status() const;
    int window_size() const { return static_cast<int>(window_.size()); }

private:
    Config cfg_;
    uint32_t session_ = 0;
    bool active_ = false;
    bool pending_ = false;
    bool missed_ = false;        // 直近の結果が「見つからず」
    uint16_t key_ = 0;
    int64_t burst_index_ = 0;    // このチャネルの秘話バースト通し番号(要求間隔の基準)
    int64_t last_request_ = -1;
    std::deque<Burst> window_;
};

} // namespace spear::std_t98::secret
