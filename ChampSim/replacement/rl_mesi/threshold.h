#ifndef REPLACEMENT_RL_MESI_THRESHOLD_H
#define REPLACEMENT_RL_MESI_THRESHOLD_H

/*
 * C1: the sharing threshold of our coherence-aware RL replacement policy.
 *
 * The paper (Souza & Freitas, IEEE Access 2024) calls a block "above threshold" iff its sharer count is greater than
 * num_cpus / 2, fixed at design time. That cannot adapt: if every block is shared by 2-3 of 16 cores, every block is below
 * threshold and the feature carries no information. Here the threshold t (heavy iff count > t) is chosen per cache by one of:
 *
 *   fixed    t = num_cpus / 2, exactly the paper.
 *   average  t = floor(EMA of the sharer counts seen at fills). Heavy means "more widely shared than usual here".
 *   duel     A multi-armed bandit over a few candidate thresholds using leader sets (set dueling, as in DIP): each candidate
 *            owns a few sets that always classify with it; followers use the candidate whose leaders miss least.
 *
 * Header-only, deterministic, independent of ChampSim and of rl_core.h so that it can be unit tested on its own.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace rlm
{
enum class threshold_mode { fixed, average, duel };

inline std::optional<threshold_mode> parse_threshold_mode(std::string_view s)
{
  if (s == "fixed")
    return threshold_mode::fixed;
  if (s == "average")
    return threshold_mode::average;
  if (s == "duel")
    return threshold_mode::duel;
  return std::nullopt;
}

inline constexpr std::string_view name(threshold_mode m)
{
  constexpr std::string_view names[] = {"fixed", "average", "duel"};
  return names[static_cast<int>(m)];
}

struct threshold_config {
  threshold_mode m = threshold_mode::duel;
  std::size_t max_candidates = 4;          // duel: K, the number of thresholds compared
  std::size_t leaders_per_candidate = 16;  // duel: requested; capped so that at least half the sets are followers
  uint64_t epoch_accesses = 8192;          // duel: leader accesses between two winner decisions
  bool decay = true;                       // duel: halve the counts at each epoch end (false: reset them)
  unsigned ema_shift = 4;                  // average: EMA rate 1 / 2^ema_shift (default 1/16)
};

/*
 * Candidate thresholds for the duel, at most max_k of them, sorted, always including the paper's num_cpus / 2.
 *   num_cpus <= max_k: every t in 0..num_cpus-1         (1 core: {0}; 2: {0,1}; 4: {0,1,2,3})
 *   otherwise:         {1, 2, mid, num_cpus/2}           (8: {1,2,4,6}; 16: {1,2,4,8}; 32: {1,2,8,16})
 * Sharer counts are skewed toward small values, so the low end is sampled densely: t = 1 separates private from truly
 * shared blocks, t = 2 separates pairs (producer/consumer) from wider sharing. mid is num_cpus/4 when that is above 2,
 * else 3*num_cpus/4. t = 0 (every block with a sharer is heavy) is only kept for tiny core counts, where it is cheap.
 * Missing slots (e.g. num_cpus = 5 gives {1,2,3}) are filled with the lowest unused values.
 */
inline std::vector<uint32_t> default_threshold_candidates(std::size_t num_cpus, std::size_t max_k)
{
  const auto n = static_cast<uint32_t>(std::max<std::size_t>(num_cpus, 1));
  const auto k = static_cast<uint32_t>(std::max<std::size_t>(max_k, 1));
  std::vector<uint32_t> c;
  if (n <= k) {
    for (uint32_t t = 0; t < n; ++t)
      c.push_back(t);
    return c;
  }
  const uint32_t mid = (n / 4 > 2) ? n / 4 : (3 * n) / 4;
  c = {1, 2, mid, n / 2};
  std::sort(c.begin(), c.end());
  c.erase(std::unique(c.begin(), c.end()), c.end());
  for (uint32_t t = 0; t < n && c.size() < k; ++t)
    if (std::find(c.begin(), c.end(), t) == c.end())
      c.push_back(t);
  std::sort(c.begin(), c.end());
  // The paper's value must survive the cut to k
  while (c.size() > k)
    c.erase(c.begin() + (c.front() == n / 2 ? 1 : 0));
  return c;
}

struct candidate_stats {
  uint32_t threshold = 0;
  std::size_t leader_sets = 0;
  uint64_t accesses = 0;  // current (decayed) window
  uint64_t misses = 0;
  uint64_t total_accesses = 0;
  uint64_t total_misses = 0;
  uint64_t wins = 0;  // epochs that ended with this candidate as the winner
};

class sharing_threshold
{
  static constexpr int FOLLOWER = -1;
  static constexpr uint32_t EMA_FRAC_BITS = 8;  // average kept in fixed point: avg * 2^8

  threshold_config cfg_;
  std::size_t num_sets_;
  uint32_t num_cpus_;
  std::vector<candidate_stats> cand_;
  std::vector<int> owner_;  // per set: candidate index of a leader set, FOLLOWER otherwise
  std::size_t leaders_per_cand_ = 0;
  std::size_t default_idx_ = 0;  // candidate closest to the paper's value; wins ties
  std::size_t winner_ = 0;
  uint64_t epoch_count_ = 0;
  uint64_t epochs_ = 0;
  int64_t avg_fp_;

  void assign_leaders(std::size_t requested)
  {
    const std::size_t k = cand_.size();
    if (k < 2)
      return;  // nothing to duel
    // Cap: at least half the sets follow, and each constituency has room for k leaders with 2 slots each
    leaders_per_cand_ = std::min(requested, num_sets_ / (2 * k));
    if (leaders_per_cand_ == 0)
      return;  // tiny cache: no dueling, followers keep the paper's value
    // Sets are cut into leaders_per_cand_ constituencies; candidate i leads one set in each, in its own sub-range,
    // shifted by the constituency number so that leaders do not all share the same low index bits
    const std::size_t stride = num_sets_ / leaders_per_cand_;
    const std::size_t sub = stride / k;
    for (std::size_t g = 0; g < leaders_per_cand_; ++g)
      for (std::size_t i = 0; i < k; ++i) {
        owner_.at(g * stride + i * sub + g % sub) = static_cast<int>(i);
        ++cand_.at(i).leader_sets;
      }
  }

  void end_epoch()
  {
    ++epochs_;
    epoch_count_ = 0;
    std::size_t best = default_idx_;
    for (std::size_t i = 0; i < cand_.size(); ++i)
      if (better(i, best))
        best = i;
    winner_ = best;
    ++cand_.at(winner_).wins;
    for (auto& c : cand_) {
      c.accesses = cfg_.decay ? c.accesses / 2 : 0;
      c.misses = cfg_.decay ? c.misses / 2 : 0;
    }
  }

  // Strictly lower miss ratio, compared exactly on integers; a group with no accesses never wins
  [[nodiscard]] bool better(std::size_t a, std::size_t b) const
  {
    const auto& x = cand_.at(a);
    const auto& y = cand_.at(b);
    if (x.accesses == 0)
      return false;
    if (y.accesses == 0)
      return true;
    return x.misses * y.accesses < y.misses * x.accesses;
  }

public:
  // candidates: duel thresholds; empty means default_threshold_candidates(num_cpus, cfg.max_candidates)
  sharing_threshold(std::size_t num_sets, std::size_t num_cpus, threshold_config cfg = {}, std::vector<uint32_t> candidates = {})
      : cfg_(cfg), num_sets_(num_sets), num_cpus_(static_cast<uint32_t>(num_cpus)), owner_(num_sets, FOLLOWER),
        avg_fp_(static_cast<int64_t>(num_cpus / 2) << EMA_FRAC_BITS)
  {
    if (num_sets == 0 || num_cpus == 0)
      throw std::invalid_argument("sharing_threshold: num_sets and num_cpus must be positive");
    if (cfg.m != threshold_mode::duel)
      return;
    if (candidates.empty())
      candidates = default_threshold_candidates(num_cpus, cfg.max_candidates);
    for (auto t : candidates)
      cand_.push_back(candidate_stats{t});
    const auto paper = static_cast<int64_t>(paper_threshold());
    for (std::size_t i = 1; i < cand_.size(); ++i)
      if (std::abs(static_cast<int64_t>(cand_.at(i).threshold) - paper) < std::abs(static_cast<int64_t>(cand_.at(default_idx_).threshold) - paper))
        default_idx_ = i;
    winner_ = default_idx_;
    assign_leaders(cfg.leaders_per_candidate);
  }

  [[nodiscard]] threshold_mode mode() const { return cfg_.m; }
  [[nodiscard]] uint32_t paper_threshold() const { return num_cpus_ / 2; }

  // The t for this set: heavy iff count > t
  [[nodiscard]] uint32_t threshold_for(std::size_t set) const
  {
    switch (cfg_.m) {
    case threshold_mode::average:
      return static_cast<uint32_t>(avg_fp_ >> EMA_FRAC_BITS);
    case threshold_mode::duel: {
      const int owner = owner_.at(set);
      return cand_.at(owner == FOLLOWER ? winner_ : static_cast<std::size_t>(owner)).threshold;
    }
    case threshold_mode::fixed:
    default:
      return paper_threshold();
    }
  }

  // The `above` feature of the RL policy
  [[nodiscard]] bool heavy(std::size_t set, uint32_t count) const { return count > threshold_for(set); }

  /*
   * average: a sharer count seen at a fill. Counts above num_cpus (corrupt or stale) are clamped. The step is rounded away
   * from zero so that the EMA converges exactly onto a constant stream: if every count is c, t becomes c and nothing is
   * heavy, which is right, since the feature then has nothing to tell apart. No-op in the other modes.
   */
  void observe(uint32_t count)
  {
    if (cfg_.m != threshold_mode::average)
      return;
    const int64_t target = static_cast<int64_t>(std::min(count, num_cpus_)) << EMA_FRAC_BITS;
    const int64_t diff = target - avg_fp_;
    int64_t step = diff / (int64_t{1} << cfg_.ema_shift);
    if (step == 0 && diff != 0)
      step = diff > 0 ? 1 : -1;
    avg_fp_ += step;
  }

  // duel: one demand access to a set (a hit, or a miss that fills). Only leader sets count. No-op in the other modes.
  void on_access(std::size_t set, bool hit)
  {
    if (cfg_.m != threshold_mode::duel)
      return;
    const int owner = owner_.at(set);
    if (owner == FOLLOWER)
      return;
    auto& c = cand_.at(static_cast<std::size_t>(owner));
    ++c.accesses;
    ++c.total_accesses;
    if (!hit) {
      ++c.misses;
      ++c.total_misses;
    }
    if (++epoch_count_ >= cfg_.epoch_accesses)
      end_epoch();
  }
  void on_miss(std::size_t set) { on_access(set, false); }

  // Candidate index leading this set, if any (duel only)
  [[nodiscard]] std::optional<std::size_t> leader_of(std::size_t set) const
  {
    const int owner = owner_.at(set);
    if (owner == FOLLOWER)
      return std::nullopt;
    return static_cast<std::size_t>(owner);
  }

  [[nodiscard]] std::size_t winner() const { return winner_; }
  [[nodiscard]] uint32_t winner_threshold() const { return cfg_.m == threshold_mode::duel ? cand_.at(winner_).threshold : threshold_for(0); }
  [[nodiscard]] const std::vector<candidate_stats>& candidates() const { return cand_; }
  [[nodiscard]] std::size_t leaders_per_candidate() const { return leaders_per_cand_; }
  [[nodiscard]] uint64_t epochs() const { return epochs_; }
  [[nodiscard]] double average() const { return static_cast<double>(avg_fp_) / (1 << EMA_FRAC_BITS); }
};
} // namespace rlm

#endif
