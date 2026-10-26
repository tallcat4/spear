#include "../channel_plan.hpp"
#include "worker.hpp"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <map>

namespace spear::std_t98::secret {

Worker::Worker(std::function<void(const Result&)> on_result, std::function<void(const std::string&)> on_log)
    : on_result_(std::move(on_result)), on_log_(std::move(on_log)) {
    th_ = std::thread([this] { run(); });
}

Worker::~Worker() {
    cancel_ = true;
    { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void Worker::submit(Request r) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!st_.ready && loaded_) return;   // モデルがロードできなかった: 受け付けない(音声は平文として復号され続ける)
        std::erase_if(queue_, [&](const Request& q) { return q.channel == r.channel; });
        queue_.push_back(std::move(r));
        ++st_.requests;
        st_.queued = queue_.size();
    }
    cv_.notify_one();
}

Worker::Status Worker::status() const { std::lock_guard<std::mutex> lk(mu_); return st_; }

void Worker::wait_ready() const {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return loaded_; });
}

void Worker::run() {
    // 実時間の DSP thread より下の優先度で走る(Linux では setpriority がスレッド単位に効く)。線形探索とhybrid推論はこのスレッドで完結する
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
    // ---- ロード(このスレッド上。GUI thread / DSP thread を待たせない) ----
    Hybrid hybrid;
    std::string err;
    const bool ok = hybrid.load_embedded(&err);
    struct Channel {
        uint32_t session = 0;
        uint64_t next = 0;
        std::unique_ptr<GuidedSession> solver;
    };
    std::map<int, Channel> channels;
    std::vector<SeriesKey> cache;
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.ready = ok;
        st_.error = ok ? std::string() : err;
        loaded_ = true;
        if (!ok) queue_.clear();
    }
    cv_.notify_all();
    if (on_log_) on_log_(ok ? std::string("secret: solver-guided AI, standard + enhanced series 1..15") : "secret disabled: " + err);
    if (!ok) return;

    // ---- 要求ループ ----
    while (true) {
        Request r;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            r = std::move(queue_.front());
            queue_.pop_front();
            st_.queued = queue_.size();
        }
        const auto t0 = std::chrono::steady_clock::now();
        Result out;
        out.channel = r.channel;
        out.session = r.session;
        try {
            if (r.channel < 0 || r.channel >= kRegisteredChannels || (!r.valid.empty() && r.valid.size() != r.bursts.size()))
                throw std::runtime_error("invalid secret request");
            auto& ch = channels[r.channel];
            // An overwritten/overrun window must never invent adjacent frames.
            if (!ch.solver || ch.session != r.session || r.first_burst > ch.next) {
                ch.solver = std::make_unique<GuidedSession>(hybrid_scorer(hybrid));
                ch.session = r.session;
                ch.next = r.first_burst;
            }
            for (std::size_t i = 0; i < r.bursts.size(); ++i) {
                if (cancel_) return;
                const auto index = r.first_burst + i;
                if (index < ch.next) continue;
                ch.solver->add(r.bursts[i], r.valid.empty() ? kAllValid : r.valid[i]);
                ch.next = index + 1;
            }
            const auto res = ch.solver->assess();
            if (res.accepted) {
                out.accepted = true;
                out.key = res.accepted->key;
                out.series = res.accepted->series;
                out.source = ResultSource::GuidedAI;
                std::erase(cache, *res.accepted);
                cache.insert(cache.begin(), *res.accepted);
                if (cache.size() > 16) cache.resize(16);
            }
        } catch (const std::exception& e) {
            channels.erase(r.channel);
            if (on_log_) on_log_(std::string("secret search failed: ") + e.what());
        }
        if (cancel_) return;
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        out.cache = cache;
        { std::lock_guard<std::mutex> lk(mu_); ++st_.results; }
        if (on_result_) on_result_(out);
    }
}

} // namespace spear::std_t98::secret
