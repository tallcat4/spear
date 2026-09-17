// spear-gui — TraceSource: 固定長トレース(アイパターン、コンスタレーションの 1 区間など)をどのスレッドからでも push でき、
// QML の item(EyeDiagramItem 等)が GUI thread でまとめて取り出す薄い QObject (要件 §9.3 の再利用部品)。
// ViewSource が「stream → spectrum」なのに対し、こちらは App の DSP が既に切り出した短いトレースを運ぶ。
#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace spear::appfw {

class TraceSource : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("created by the application")
    Q_PROPERTY(int traceLength READ traceLength NOTIFY formatChanged)
    Q_PROPERTY(int capacity READ capacity WRITE setCapacity NOTIFY formatChanged)
    Q_PROPERTY(double tracesPerSecond READ tracesPerSecond NOTIFY formatChanged)
public:
    explicit TraceSource(QObject* parent = nullptr) : QObject(parent) {}

    // producer(任意スレッド): 1 トレース追加。長さが変わるとリングをリセットする
    void push(std::span<const float> trace) {
        std::lock_guard<std::mutex> lk(mu_);
        if (trace.size() != len_) { len_ = trace.size(); ring_.assign(len_ * static_cast<std::size_t>(cap_), 0.f); count_ = 0; head_ = 0; fmt_dirty_ = true; }
        std::copy(trace.begin(), trace.end(), ring_.begin() + static_cast<long>(head_ * len_));
        head_ = (head_ + 1) % static_cast<std::size_t>(cap_);
        if (count_ < static_cast<std::size_t>(cap_)) ++count_;
        ++seq_;
    }
    void clear() { std::lock_guard<std::mutex> lk(mu_); count_ = 0; head_ = 0; ++seq_; }

    // consumer(GUI thread): 新しいトレースがあれば全リングを古い順にコピーして true
    bool snapshot(std::vector<float>& flat, std::size_t& trace_len, std::size_t& n_traces, uint64_t& seen) {
        std::lock_guard<std::mutex> lk(mu_);
        if (seq_ == seen) return false;
        seen = seq_;
        trace_len = len_;
        n_traces = count_;
        flat.resize(len_ * count_);
        if (len_ == 0 || count_ == 0) return true;
        // 古い順: head_ − count_ から
        const std::size_t cap = static_cast<std::size_t>(cap_);
        std::size_t src = (head_ + cap - count_) % cap;
        for (std::size_t i = 0; i < count_; ++i) {
            std::copy_n(ring_.begin() + static_cast<long>(src * len_), len_, flat.begin() + static_cast<long>(i * len_));
            src = (src + 1) % cap;
        }
        return true;
    }
    void set_rate(double traces_per_second) { rate_ = traces_per_second; }

    int traceLength() const { std::lock_guard<std::mutex> lk(mu_); return static_cast<int>(len_); }
    int capacity() const { return cap_; }
    void setCapacity(int n) {
        std::lock_guard<std::mutex> lk(mu_);
        cap_ = std::max(1, std::min(n, 4096));
        ring_.assign(len_ * static_cast<std::size_t>(cap_), 0.f); count_ = 0; head_ = 0;
        Q_EMIT formatChanged();
    }
    double tracesPerSecond() const { return rate_; }
    bool take_format_dirty() { std::lock_guard<std::mutex> lk(mu_); const bool d = fmt_dirty_; fmt_dirty_ = false; return d; }

Q_SIGNALS:
    void formatChanged();

private:
    mutable std::mutex mu_;
    std::vector<float> ring_;
    std::size_t len_ = 0, head_ = 0, count_ = 0;
    int cap_ = 256;
    uint64_t seq_ = 0;
    bool fmt_dirty_ = false;
    double rate_ = 0;
};

} // namespace spear::appfw
