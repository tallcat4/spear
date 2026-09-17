// S.P.E.A.R. core — Sample Block / BlockRef / BlockPool (要件 §6)
//
// 守るべき1点 (§6.2): Block 参照は opaque handle。
//   BlockRef の内部表現(現在は heap pool + shared_ptr<const>)は公開 API から見えない。
//   将来 shared memory / zero-copy 化しても利用側は変わらない。
// immutability: commit 後の block は const でしか触れない (§12.2)。
#pragma once

#include "types.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace spear {

class BlockPool;
class BlockBuilder;

namespace detail {
// 内部表現。利用側はこれに触れない。
struct Block {
    BlockHeader              header;
    std::span<std::byte>     storage;   // pool slot または heap fallback
    BlockPool*               owner = nullptr;
    int64_t                  slot  = -1; // -1 = heap fallback
    std::unique_ptr<std::byte[]> heap;   // fallback 時のみ所有
};
} // namespace detail

// immutable な Sample Block への参照。コピーは参照カウントのみ(データはコピーしない)。
class BlockRef {
public:
    BlockRef() = default;

    bool valid() const noexcept { return static_cast<bool>(p_); }
    explicit operator bool() const noexcept { return valid(); }

    const BlockHeader& header() const noexcept { assert(p_); return p_->header; }

    std::span<const std::byte> bytes() const noexcept {
        assert(p_);
        return {p_->storage.data(), p_->header.sample_count * dtype_size(p_->header.dtype)};
    }

    // 型付きアクセス。dtype 不一致は契約違反(assert)。
    template <class T>
    std::span<const T> as() const noexcept {
        assert(p_ && p_->header.dtype == dtype_of<T>());
        return {reinterpret_cast<const T*>(p_->storage.data()), p_->header.sample_count};
    }

    SampleRange range() const noexcept {
        const auto& h = header();
        return {h.generation, h.sample_index, h.sample_end()};
    }

private:
    friend class BlockBuilder;
    explicit BlockRef(std::shared_ptr<const detail::Block> p) : p_(std::move(p)) {}
    std::shared_ptr<const detail::Block> p_;
};

// 書き込み中の block。move-only、単一所有。commit() で immutable な BlockRef になる。
class BlockBuilder {
public:
    BlockBuilder() = default;
    BlockBuilder(BlockBuilder&&) noexcept = default;
    BlockBuilder& operator=(BlockBuilder&&) noexcept = default;
    BlockBuilder(const BlockBuilder&) = delete;
    BlockBuilder& operator=(const BlockBuilder&) = delete;
    ~BlockBuilder(); // commit されなかった block は pool へ返す

    bool valid() const noexcept { return static_cast<bool>(b_); }
    explicit operator bool() const noexcept { return valid(); }

    BlockHeader& header() noexcept { assert(b_); return b_->header; }

    template <class T>
    std::span<T> data() noexcept {
        assert(b_);
        return {reinterpret_cast<T*>(b_->storage.data()), b_->storage.size() / sizeof(T)};
    }
    std::span<std::byte> bytes() noexcept { assert(b_); return b_->storage; }

    // sample_count 個の sample を確定し、immutable 参照を返す。以後 builder は空。
    BlockRef commit(uint32_t sample_count) noexcept;

private:
    friend class BlockPool;
    explicit BlockBuilder(detail::Block* b) : b_(b) {}
    detail::Block* b_ = nullptr;
};

struct PoolStats {
    std::size_t capacity       = 0; // slot 数
    std::size_t slot_bytes     = 0;
    std::size_t in_use         = 0;
    std::size_t peak_in_use    = 0;
    uint64_t    heap_fallbacks = 0; // pool 枯渇時の heap 確保回数。0 でなければ consumer が block を抱えすぎ
};

// 固定 slot 数の buffer pool。枯渇時は heap fallback(無言で失敗しない: 統計に出す)。
// 40 MB/s では素朴な mutex 実装で足りる (§6.1)。lock-free 化は要件で固定しない。
class BlockPool {
public:
    BlockPool(std::size_t slot_bytes, std::size_t slots);
    ~BlockPool();
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    // dtype と最大 sample 数を指定して書き込み用 block を得る。
    BlockBuilder acquire(DataType dtype, std::size_t max_samples);

    template <class T>
    BlockBuilder acquire(std::size_t max_samples) { return acquire(dtype_of<T>(), max_samples); }

    PoolStats stats() const;
    std::size_t slot_bytes() const noexcept { return slot_bytes_; }

private:
    friend class BlockBuilder;
    void release(detail::Block* b) noexcept;

    const std::size_t slot_bytes_;
    std::vector<std::byte> arena_;
    std::vector<detail::Block> blocks_;      // slot ごとの record(再利用)
    mutable std::mutex mu_;
    std::vector<int64_t> free_;
    std::size_t in_use_ = 0, peak_ = 0;
    uint64_t heap_fallbacks_ = 0;
};

} // namespace spear
