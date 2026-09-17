#include "worker.hpp"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>

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
    // 実時間の DSP thread より下の優先度で走る(Linux では setpriority がスレッド単位に効く)。全鍵探索は 1 コアを約 1 s 占有する
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
    // ---- ロード(このスレッド上。GUI thread / DSP thread を待たせない) ----
    Ffnn ffnn;
    Hybrid hybrid;
    std::string err;
    const bool ok = ffnn.load_embedded(&err) && hybrid.load_embedded(&err);
    std::unique_ptr<Cracker> cracker;
    if (ok) cracker = std::make_unique<Cracker>(ffnn, hybrid);
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.ready = ok;
        st_.error = ok ? std::string() : err;
        loaded_ = true;
        if (!ok) queue_.clear();
    }
    cv_.notify_all();
    if (on_log_) on_log_(ok ? std::string("secret: models ready (ffnn C++, hybrid onnxruntime)") : "secret disabled: " + err);
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
        const Resolution res = cracker->resolve(r.current_key, r.bursts, &cancel_);
        if (cancel_) return;
        Result out;
        out.channel = r.channel;
        out.session = r.session;
        out.key = res.key;
        out.source = res.source;
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        out.cache = res.cache;
        { std::lock_guard<std::mutex> lk(mu_); ++st_.results; }
        if (on_result_) on_result_(out);
    }
}

} // namespace spear::std_t98::secret
