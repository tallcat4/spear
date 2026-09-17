#include "cracker.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace spear::std_t98::secret {

const char* to_string(ResultSource s) {
    switch (s) {
    case ResultSource::None: return "search miss";
    case ResultSource::CurrentKey: return "current key verified";
    case ResultSource::GlobalCache: return "global cache hit";
    case ResultSource::FullSearch: return "full search hit";
    }
    return "?";
}

Cracker::Cracker(const Ffnn& ffnn, const Hybrid& hybrid, Config cfg) : ffnn_(ffnn), hybrid_(hybrid), cfg_(cfg) {
    pnm_.resize(static_cast<std::size_t>(kMaxKey) * kPnBits);
    for (uint32_t key = 1; key <= kMaxKey; ++key) {
        const Pn196 pn = pn_sequence(static_cast<uint16_t>(key));
        uint8_t* row = &pnm_[static_cast<std::size_t>(key - 1) * kPnBits];
        for (int f = 0; f < kFramesPerBurst; ++f) {
            const Frame49 ks = keystream_thumbdv(pn, f);
            std::copy(ks.begin(), ks.end(), row + f * kBitsPerFrame);
        }
    }
}

std::vector<Cracker::Block> Cracker::to_blocks(std::span<const Burst> bursts) {
    std::vector<Block> out;
    for (std::size_t start = 0; start + kBurstsPerBlock <= bursts.size() && out.size() < 2; start += kBurstsPerBlock) {
        Block b;
        for (int i = 0; i < kBurstsPerBlock; ++i)
            for (int f = 0; f < kFramesPerBurst; ++f) b[static_cast<std::size_t>(i * kFramesPerBurst + f)] = bursts[start + static_cast<std::size_t>(i)][static_cast<std::size_t>(f)];
        out.push_back(b);
    }
    return out;
}

void Cracker::descramble(const Block& raw, uint16_t key, uint8_t out[kInputDim]) const {
    const uint8_t* row = key ? &pnm_[static_cast<std::size_t>(key - 1) * kPnBits] : nullptr;
    for (int fr = 0; fr < kFramesPerBlock; ++fr) {
        const uint8_t* src = raw[static_cast<std::size_t>(fr)].data();
        uint8_t* dst = out + fr * kBitsPerFrame;
        if (!row) { std::copy(src, src + kBitsPerFrame, dst); continue; }
        const uint8_t* ks = row + (fr % kFramesPerBurst) * kBitsPerFrame;
        for (int i = 0; i < kBitsPerFrame; ++i) dst[i] = static_cast<uint8_t>(src[i] ^ ks[i]);
    }
}

float Cracker::ffnn_margin(const Block& raw, uint16_t key) const {
    uint8_t x[kInputDim];
    descramble(raw, key, x);
    return ffnn_.margin(x);
}

Resolution Cracker::resolve(uint16_t current_key, std::span<const Burst> bursts, const std::atomic<bool>* cancel) {
    const auto blocks = to_blocks(bursts);
    if (blocks.empty()) return {0, ResultSource::None, cache_};

    if (current_key > 0) {
        const uint16_t k = verify_candidates(blocks, std::span<const uint16_t>(&current_key, 1));
        if (k) { remember(k); return {k, ResultSource::CurrentKey, cache_}; }
    }
    std::vector<uint16_t> cands;
    for (uint16_t k : cache_) if (k != current_key && static_cast<int>(cands.size()) < cfg_.cache_verify_limit) cands.push_back(k);
    if (const uint16_t k = verify_candidates(blocks, cands)) { remember(k); return {k, ResultSource::GlobalCache, cache_}; }

    if (const uint16_t k = full_search(blocks, cancel)) { remember(k); return {k, ResultSource::FullSearch, cache_}; }
    return {0, ResultSource::None, cache_};
}

void Cracker::remember(uint16_t key) {
    if (!key) return;
    std::erase(cache_, key);
    cache_.insert(cache_.begin(), key);
    if (static_cast<int>(cache_.size()) > cfg_.global_cache_limit) cache_.resize(static_cast<std::size_t>(cfg_.global_cache_limit));
}

std::pair<int, float> Cracker::score_candidate(std::span<const Block> blocks, uint16_t key) const {
    int votes = 0;
    float best = -INFINITY;
    for (const auto& b : blocks) {
        const float m = ffnn_margin(b, key);
        if (m > 0.f) ++votes;   // argmax == plain ⇔ logit0 > logit1(同点は torch では index 0 = plain だが、float で同点は事実上ない)
        best = std::max(best, m);
    }
    return {votes, best};
}

uint16_t Cracker::verify_candidates(std::span<const Block> blocks, std::span<const uint16_t> keys) const {
    // Python: (plain_votes, best_margin, key) の辞書順最大
    bool have = false;
    int best_votes = 0; float best_margin = 0; uint16_t best_key = 0;
    for (uint16_t key : keys) {
        if (key == 0 || key > kMaxKey) continue;
        const auto [votes, margin] = score_candidate(blocks, key);
        if (votes <= 0) continue;
        const bool better = !have || votes > best_votes || (votes == best_votes && (margin > best_margin || (margin == best_margin && key > best_key)));
        if (better) { have = true; best_votes = votes; best_margin = margin; best_key = key; }
    }
    return have ? best_key : 0;
}

Cracker::Scored Cracker::stage_scores(const Block& raw, std::span<const uint32_t> keys) const {
    Scored s;
    s.score.resize(keys.size());
    s.prob.resize(keys.size());
    std::vector<float> in, out;
    uint8_t x[kInputDim];
    for (std::size_t start = 0; start < keys.size(); start += static_cast<std::size_t>(cfg_.hybrid_batch)) {
        const std::size_t n = std::min(static_cast<std::size_t>(cfg_.hybrid_batch), keys.size() - start);
        in.resize(n * kInputDim);
        out.resize(n * 2);
        for (std::size_t j = 0; j < n; ++j) {
            descramble(raw, static_cast<uint16_t>(keys[start + j]), x);
            for (int i = 0; i < kInputDim; ++i) in[j * kInputDim + static_cast<std::size_t>(i)] = static_cast<float>(x[i]);
        }
        if (!hybrid_.logits(in.data(), n, out.data())) { std::fill(s.score.begin(), s.score.end(), 0.f); std::fill(s.prob.begin(), s.prob.end(), 0.5f); return s; }
        for (std::size_t j = 0; j < n; ++j) {
            const float l0 = out[j * 2], l1 = out[j * 2 + 1];
            s.score[start + j] = l0 - l1;
            s.prob[start + j] = 1.f / (1.f + std::exp(l1 - l0));   // softmax の平文確率
        }
    }
    return s;
}

uint16_t Cracker::full_search(std::span<const Block> blocks, const std::atomic<bool>* cancel) const {
    // 0) そもそも平文か(hybrid)。平文なら鍵なし
    {
        float in[kInputDim], out[2];
        for (int i = 0; i < kInputDim; ++i) in[i] = static_cast<float>(blocks[0][static_cast<std::size_t>(i / kBitsPerFrame)][static_cast<std::size_t>(i % kBitsPerFrame)]);
        if (hybrid_.logits(in, 1, out) && out[kLabelPlain] > out[kLabelEncrypted]) return 0;
    }
    // 1) 全鍵を ffnn でふるう(どちらかのブロックで平文と判定された鍵が候補)
    std::vector<uint32_t> candidates;
    for (uint32_t key = 1; key <= kMaxKey; ++key) {
        if (cancel && (key & 1023u) == 0 && cancel->load(std::memory_order_relaxed)) return 0;
        bool plain = ffnn_margin(blocks[0], static_cast<uint16_t>(key)) > 0.f;
        if (!plain && blocks.size() > 1) plain = ffnn_margin(blocks[1], static_cast<uint16_t>(key)) > 0.f;
        if (plain) candidates.push_back(key);
    }
    if (candidates.empty()) return 0;
    // 2) ブロック 1 を hybrid で採点。score > 0 が生存、5 未満なら上位 5 で補う
    const Scored s2 = stage_scores(blocks[0], candidates);
    std::vector<std::size_t> survivors;
    for (std::size_t j = 0; j < candidates.size(); ++j) if (s2.score[j] > 0.f) survivors.push_back(j);
    if (survivors.size() < 5) {
        std::vector<std::size_t> order(candidates.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return s2.score[a] > s2.score[b]; });
        order.resize(std::min<std::size_t>(5, order.size()));
        survivors = order;
    }
    if (survivors.empty()) return 0;
    if (blocks.size() < 2) {
        std::size_t best = survivors[0];
        for (std::size_t j : survivors) if (s2.score[j] > s2.score[best]) best = j;
        return static_cast<uint16_t>(candidates[best]);
    }
    // 3) ブロック 2 で順位付け: (score3 > 0, score2, prob2, prob3) の辞書順で最大
    std::vector<uint32_t> keys3;
    for (std::size_t j : survivors) keys3.push_back(candidates[j]);
    const Scored s3 = stage_scores(blocks[1], keys3);
    std::size_t best = 0;
    auto better = [&](std::size_t a, std::size_t b) {   // a が b より良いか
        const bool pa = s3.score[a] > 0.f, pb = s3.score[b] > 0.f;
        if (pa != pb) return pa;
        const std::size_t ca = survivors[a], cb = survivors[b];
        if (s2.score[ca] != s2.score[cb]) return s2.score[ca] > s2.score[cb];
        if (s2.prob[ca] != s2.prob[cb]) return s2.prob[ca] > s2.prob[cb];
        return s3.prob[a] > s3.prob[b];
    };
    for (std::size_t a = 1; a < survivors.size(); ++a) if (better(a, best)) best = a;
    return static_cast<uint16_t>(keys3[best]);
}

} // namespace spear::std_t98::secret
