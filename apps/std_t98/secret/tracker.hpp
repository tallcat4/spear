// チャネル/PTTの要求管理。GUIはstreaming=trueで使用する。
// 窓を絶対バースト番号とFEC正常フラグ付きで渡す。旧ベンチ用の窓方式も保持。
// guided結果は未確定なら鍵を取り消し、PTTをまたいで鍵を適用しない。
#pragma once
#include "pn.hpp"
#include "guided.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace spear::std_t98::secret {

enum class TrackerStatus : uint8_t { Idle, Collecting, Pending, Keyed, Miss };
const char* to_string(TrackerStatus s);

struct TrackerConfig {
    bool streaming = false; // Guided AI: overlap windows with absolute burst offsets
    int min_window = 5;
    int max_window = 10;
    int recheck_interval = 5;  // 軽量なソルバー単独評価では短い PTT 中にも再確認する
};

class Tracker {
public:
    using Config = TrackerConfig;
    struct Request {
        uint32_t session = 0;
        uint16_t current_key = 0;
        std::vector<Burst> bursts;
        uint64_t first_burst = 0;
        std::vector<Valid4> valid;
    };
    Tracker(Config cfg = Config()) : cfg_(cfg) {}   // NOLINT: SecretChan{} の集約初期化で使う

    // 秘話 TCH フレームを 1 つ受け取った。要求を出すべきなら true と request を返す(呼び出し側がワーカーへ渡す)
    bool on_secret_burst(const Burst& b, Request* request, const Valid4& valid = kAllValid);
    // 平文フレーム / 同期バースト / 同期喪失 → セッション終了(鍵は保持)
    void on_clear();
    // 探索結果。セッションが違えば無視
    void on_result(uint32_t session, uint16_t key);

    void on_guided_result(uint32_t session, std::optional<SeriesKey> id);
    uint8_t series() const { return series_; }
    bool confirmed() const { return confirmed_; }

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
    uint8_t series_ = 0;
    bool confirmed_ = false;
    uint64_t session_bursts_ = 0;
    std::deque<Valid4> validity_;
    uint16_t key_ = 0;
    int64_t burst_index_ = 0;    // このチャネルの秘話バースト通し番号(要求間隔の基準)
    int64_t last_request_ = -1;
    std::deque<Burst> window_;
};

} // namespace spear::std_t98::secret
