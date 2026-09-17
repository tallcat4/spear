// 鍵探索ワーカー — 専用スレッド 1 本でモデルをロードし、要求を順に Cracker で解く(DSP thread では走らせない)。
// tools では別プロセス(std_t98_multi_secret_service.py)+ UDS だったものを、単一プロセス内のキュー + コールバックにした(要件 §3.4)。
// 要求は同じチャネルの未処理分を置き換える(古い窓を解いても意味がない)。結果コールバックはワーカースレッド上で呼ばれる。
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
    };
    struct Result {
        int channel = 0;
        uint32_t session = 0;
        uint16_t key = 0;
        ResultSource source = ResultSource::None;
        double seconds = 0;              // 探索に要した時間
        std::vector<uint16_t> cache;     // 全チャネル共通のキャッシュ(新しい順)
    };
    struct Status {
        bool ready = false;              // 両モデルがロードでき、要求を受け付けられる
        std::string error;               // ロード失敗の理由(ready = false のとき。埋め込みモデルなので通常は起きない)
        uint64_t requests = 0, results = 0, queued = 0;
    };
    // 埋め込みモデル(secret/models.hpp)をワーカースレッド上でロードする。
    explicit Worker(std::function<void(const Result&)> on_result, std::function<void(const std::string&)> on_log = {});
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void submit(Request r);
    Status status() const;
    void wait_ready() const;   // テスト用: モデルのロード完了(成否どちらでも)まで待つ

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
