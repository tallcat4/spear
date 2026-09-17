// 秘話鍵探索 — ../std-t98-tools/core/secret/cracker.py(SecretCracker)の移植。
//
// 入力: 直近の秘話 TCH フレーム(FEC 後・スクランブル解除前の 49 bit × 4)を 5 バースト = 1 ブロック(980 bit)にまとめたもの(最大 2 ブロック)。
// 手順(Python と同じ順・同じ判定):
//   1. 今の鍵(current_key)を ffnn で検証(平文票 > 0 なら採用)
//   2. 全チャネル共通の鍵キャッシュ(直近 16、上位 2 つを検証)
//   3. 全鍵探索: hybrid で「そもそも平文か」→ 全 32767 鍵を ffnn でふるう → 候補を hybrid で採点 → ブロック 2 で順位付け
// 鍵 0 = 見つからず(平文として復号する)。
#pragma once
#include "ffnn.hpp"
#include "hybrid.hpp"
#include "pn.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace spear::std_t98::secret {

inline constexpr int kBurstsPerBlock = 5;

enum class ResultSource : uint8_t { None = 0, CurrentKey = 1, GlobalCache = 2, FullSearch = 3 };
const char* to_string(ResultSource s);

struct Resolution {
    uint16_t key = 0;                 // 0 = 見つからず
    ResultSource source = ResultSource::None;
    std::vector<uint16_t> cache;      // 探索後のキャッシュ(新しい順)
};

struct CrackerConfig {
    int global_cache_limit = 16;
    int cache_verify_limit = 2;
    int hybrid_batch = 512;
};

class Cracker {
public:
    using Config = CrackerConfig;
    Cracker(const Ffnn& ffnn, const Hybrid& hybrid, Config cfg = Config());   // 両モデルともロード済みであること
    // cancel が true になったら全鍵探索を打ち切って「見つからず」を返す(App 停止時にワーカーを待たせない)
    Resolution resolve(uint16_t current_key, std::span<const Burst> bursts, const std::atomic<bool>* cancel = nullptr);
    const std::vector<uint16_t>& cache() const { return cache_; }
    void clear_cache() { cache_.clear(); }

    // ---- テスト・ツール向け ----
    using Block = std::array<Frame49, kFramesPerBlock>;   // バースト順 × フレーム順
    static std::vector<Block> to_blocks(std::span<const Burst> bursts);   // 先頭から 5 ずつ、最大 2 ブロック(cracker.py _payload_to_blocks)
    void descramble(const Block& raw, uint16_t key, uint8_t out[kInputDim]) const;   // key 0 → そのまま
    float ffnn_margin(const Block& raw, uint16_t key) const;

private:
    struct Scored { std::vector<float> score, prob; };
    std::pair<int, float> score_candidate(std::span<const Block> blocks, uint16_t key) const;   // (平文票, 最大 margin)
    uint16_t verify_candidates(std::span<const Block> blocks, std::span<const uint16_t> keys) const;
    uint16_t full_search(std::span<const Block> blocks, const std::atomic<bool>* cancel) const;
    Scored stage_scores(const Block& raw, std::span<const uint32_t> keys) const;   // hybrid の plain−enc と P(plain)
    void remember(uint16_t key);

    const Ffnn& ffnn_;
    const Hybrid& hybrid_;
    Config cfg_;
    std::vector<uint8_t> pnm_;        // [kMaxKey][196]: 鍵 k の PN を ThumbDV 順に並べ替えたもの(cracker.py の key_mapped)
    std::vector<uint16_t> cache_;     // 全チャネル共通(新しい順)
};

} // namespace spear::std_t98::secret
