// ソルバー誘導AIの専用ワーカー。チャネル/PTTごとに証拠と直近3区間を保持。
// 要求の重複窓はfirst_burstで除外し、欠落時は連続性をリセットする。
#pragma once
#include "cracker.hpp"
#include "tracker.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace spear::std_t98::secret {

class Worker {
public:
    struct Request {
        int channel = 0;
        uint32_t session = 0;
        uint16_t current_key = 0;
        std::vector<Burst> bursts;
        uint64_t first_burst = 0;
        std::vector<Valid4> valid;
    };
    struct Result {
        int channel = 0;
        uint32_t session = 0;
        uint16_t key = 0;
        ResultSource source = ResultSource::None;
        double seconds = 0;              // 探索に要した時間
        bool accepted = false;
        uint8_t series = 0;
        std::vector<SeriesKey> cache;     // 全チャネル共通のキャッシュ(新しい順)
    };
    struct Status {
        bool ready = false;              // 初期化でき、要求を受け付けられる
        std::string error;               // 初期化失敗の理由(ready = false のとき)
        uint64_t requests = 0, results = 0, queued = 0;
    };
    // 埋め込みhybridモデルをワーカースレッド上でロードする。
    explicit Worker(std::function<void(const Result&)> on_result, std::function<void(const std::string&)> on_log = {});
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void submit(Request r);
    Status status() const;
    void wait_ready() const;   // テスト用: 初期化完了(成否どちらでも)まで待つ

private:
    void run();
    std::function<void(const Result&)> on_result_;
    std::function<void(const std::string&)> on_log_;
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    std::deque<Request> queue_;
    Status st_;
    bool loaded_ = false, stop_ = false;
    std::atomic<bool> cancel_{false};
    std::thread th_;
};

} // namespace spear::std_t98::secret
