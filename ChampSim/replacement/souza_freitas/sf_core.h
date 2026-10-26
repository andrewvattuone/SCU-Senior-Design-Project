#ifndef REPLACEMENT_SOUZA_FREITAS_CORE_H
#define REPLACEMENT_SOUZA_FREITAS_CORE_H

/*
 * ChampSim-independent core of the Souza & Freitas RL replacement policy.
 *
 * M. A. Souza and H. C. Freitas, "Reinforcement Learning-Based Cache Replacement Policies for Multicore Processors,"
 * IEEE Access, vol. 12, 2024.
 *
 * Everything here is pure and header-only so that it can be unit tested without building a cache.
 */

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sf
{
enum class mode { off, state, bitset, both };
enum class base_policy { lru, srrip };
enum class rule { text, example };

// Mirrors the numeric values of champsim::coherence_state
enum class mesi : uint8_t { I = 0, S, E, M };

inline std::optional<mode> parse_mode(std::string_view s)
{
  if (s == "off")
    return mode::off;
  if (s == "state")
    return mode::state;
  if (s == "bitset")
    return mode::bitset;
  if (s == "both")
    return mode::both;
  return std::nullopt;
}

inline std::optional<base_policy> parse_base(std::string_view s)
{
  if (s == "lru")
    return base_policy::lru;
  if (s == "srrip")
    return base_policy::srrip;
  return std::nullopt;
}

inline std::optional<rule> parse_rule(std::string_view s)
{
  if (s == "text")
    return rule::text;
  if (s == "example")
    return rule::example;
  return std::nullopt;
}

inline constexpr std::string_view name(mode m)
{
  constexpr std::string_view names[] = {"off", "state", "bitset", "both"};
  return names[static_cast<int>(m)];
}
inline constexpr std::string_view name(base_policy b) { return b == base_policy::lru ? "lru" : "srrip"; }
inline constexpr std::string_view name(rule r) { return r == rule::text ? "text" : "example"; }

// A block is above threshold if MORE than half of the cores share it
inline bool above_threshold(uint64_t sharer_mask, std::size_t num_cpus) { return std::bitset<64>{sharer_mask}.count() > num_cpus / 2; }

struct history_entry {
  uint64_t tag;
  mesi state;
  bool above;
};

/*
 * Per-set Eviction History Table. Entries are kept in update order: front() is the least recently updated.
 */
class eviction_history
{
  std::size_t capacity_;
  std::vector<history_entry> entries_;

public:
  explicit eviction_history(std::size_t capacity) : capacity_(capacity) { entries_.reserve(capacity); }

  [[nodiscard]] std::size_t capacity() const { return capacity_; }
  [[nodiscard]] std::size_t size() const { return std::size(entries_); }
  [[nodiscard]] bool empty() const { return std::empty(entries_); }
  [[nodiscard]] const std::vector<history_entry>& entries() const { return entries_; }

  [[nodiscard]] bool contains(uint64_t tag) const
  {
    return std::any_of(std::cbegin(entries_), std::cend(entries_), [tag](const auto& e) { return e.tag == tag; });
  }

  // Remove the entry for this tag; returns true if it was present
  bool remove(uint64_t tag)
  {
    auto it = std::find_if(std::begin(entries_), std::end(entries_), [tag](const auto& e) { return e.tag == tag; });
    if (it == std::end(entries_))
      return false;
    entries_.erase(it);
    return true;
  }

  // Record an evicted block; returns true if the least recently updated entry had to be dropped to make room
  bool insert(uint64_t tag, mesi state, bool above)
  {
    remove(tag); // a re-recorded tag becomes the most recently updated entry
    bool dropped = false;
    if (capacity_ == 0)
      return false;
    if (std::size(entries_) >= capacity_) {
      entries_.erase(std::begin(entries_));
      dropped = true;
    }
    entries_.push_back({tag, state, above});
    return dropped;
  }
};

/*
 * Weight vector (§II-B): each weight is the fraction of history entries in that class, so each group sums to 1.
 * State weights are computed over the entries whose state is known (not I); AT/BT over all entries.
 */
struct weights {
  double m = 0, e = 0, s = 0;
  double at = 0, bt = 0;
  bool state_empty = true;
  bool bitset_empty = true;
};

inline weights compute_weights(const eviction_history& h)
{
  std::size_t nm = 0, ne = 0, ns = 0, nat = 0;
  for (const auto& entry : h.entries()) {
    nm += entry.state == mesi::M;
    ne += entry.state == mesi::E;
    ns += entry.state == mesi::S;
    nat += entry.above;
  }

  weights w;
  if (auto known = nm + ne + ns; known > 0) {
    w.m = static_cast<double>(nm) / static_cast<double>(known);
    w.e = static_cast<double>(ne) / static_cast<double>(known);
    w.s = static_cast<double>(ns) / static_cast<double>(known);
    w.state_empty = false;
  }
  if (!h.empty()) {
    w.at = static_cast<double>(nat) / static_cast<double>(h.size());
    w.bt = 1.0 - w.at;
    w.bitset_empty = false;
  }
  return w;
}

// The "highest" test counts ties as highest
inline bool state_is_highest(const weights& w, mesi state)
{
  double mine = state == mesi::M ? w.m : (state == mesi::E ? w.e : w.s);
  return mine >= std::max({w.m, w.e, w.s});
}

inline bool class_is_highest(const weights& w, bool above) { return above ? (w.at >= w.bt) : (w.bt >= w.at); }

// RULE=text evicts high-probability elements; RULE=example keeps the highest-weight element
inline bool apply_rule(rule r, bool is_highest) { return r == rule::text ? is_highest : !is_highest; }

// State check. No information (I) means accept; an empty history uses the initial bias (evict E).
inline bool state_says_evict(const weights& w, rule r, mesi state)
{
  if (state == mesi::I)
    return true;
  if (w.state_empty)
    return state == mesi::E;
  return apply_rule(r, state_is_highest(w, state));
}

// Bitset check. An empty history uses the initial bias (evict below threshold).
inline bool bitset_says_evict(const weights& w, rule r, bool above)
{
  if (w.bitset_empty)
    return !above;
  return apply_rule(r, class_is_highest(w, above));
}

inline bool should_evict(mode m, rule r, const weights& w, mesi state, bool above)
{
  switch (m) {
  case mode::state:
    return state_says_evict(w, r, state);
  case mode::bitset:
    return bitset_says_evict(w, r, above);
  case mode::both:
    // Our interpretation: evict only if BOTH checks agree
    return state_says_evict(w, r, state) && bitset_says_evict(w, r, above);
  case mode::off:
  default:
    return true;
  }
}

struct candidate_info {
  mesi state;
  bool above;
};

struct statistics {
  uint64_t decisions = 0;
  uint64_t first_accepted = 0;
  uint64_t second_chances = 0;
  uint64_t empty_history_decisions = 0;
  uint64_t history_hits = 0;     // reward -1
  uint64_t positive_rewards = 0; // reward +1
  uint64_t refill_removals = 0;
  uint64_t history_age_evictions = 0;
  uint64_t weighted_decisions = 0;
  double sum_m = 0, sum_e = 0, sum_s = 0, sum_at = 0, sum_bt = 0;
};

/*
 * Victim selection (§II-C, §II-D): the base policy proposes candidate #1; if the RL check rejects it, the base policy's
 * second choice is evicted unconditionally (only one second chance).
 *   first()        -> long             base policy's first choice
 *   second(long)   -> long             base policy's choice excluding the given way
 *   info(long)     -> candidate_info   coherence information for a way
 */
template <typename First, typename Second, typename Info>
long select_victim(mode m, rule r, const eviction_history& h, long num_ways, First&& first, Second&& second, Info&& info, statistics& stats)
{
  auto c1 = first();
  if (m == mode::off || num_ways < 2)
    return c1;

  ++stats.decisions;
  auto w = compute_weights(h);
  if (h.empty()) {
    ++stats.empty_history_decisions;
  } else {
    ++stats.weighted_decisions;
    stats.sum_m += w.m;
    stats.sum_e += w.e;
    stats.sum_s += w.s;
    stats.sum_at += w.at;
    stats.sum_bt += w.bt;
  }

  candidate_info ci = info(c1);
  if (should_evict(m, r, w, ci.state, ci.above)) {
    ++stats.first_accepted;
    return c1;
  }
  ++stats.second_chances;
  return second(c1);
}

/*
 * Reward on an access (§II-A/B): a block found in the history was a bad eviction (-1) and its entry is removed;
 * otherwise +1. Returns the reward.
 */
inline int observe_access(eviction_history& h, uint64_t tag, statistics& stats)
{
  if (h.remove(tag)) {
    ++stats.history_hits;
    return -1;
  }
  ++stats.positive_rewards;
  return +1;
}

/*
 * LRU ordering helper: identical to replacement/lru (per-way last-use stamps, the minimum is the victim, ties to the lowest way).
 */
struct lru_set {
  std::vector<uint64_t> last_used;

  explicit lru_set(long ways) : last_used(static_cast<std::size_t>(ways), 0) {}

  void touch(long way, uint64_t stamp) { last_used.at(static_cast<std::size_t>(way)) = stamp; }

  // Least recently used way, skipping `exclude` (pass -1 to exclude nothing)
  [[nodiscard]] long victim(long exclude = -1) const
  {
    long best = -1;
    for (long way = 0; way < static_cast<long>(std::size(last_used)); ++way) {
      if (way == exclude)
        continue;
      if (best < 0 || last_used.at(static_cast<std::size_t>(way)) < last_used.at(static_cast<std::size_t>(best)))
        best = way;
    }
    return best;
  }
};

/*
 * SRRIP helper: identical to replacement/srrip for the first choice. The second choice protects the rejected way by lowering its
 * RRPV below the maximum and searches again among the other ways, aging them as SRRIP normally does.
 */
struct srrip_set {
  using rrpv_type = int;
  static constexpr rrpv_type maxRRPV = 3;

  std::vector<rrpv_type> rrpv;

  explicit srrip_set(long ways) : rrpv(static_cast<std::size_t>(ways), maxRRPV) {}

  rrpv_type& get(long way) { return rrpv.at(static_cast<std::size_t>(way)); }

  void update(long way, bool hit) { get(way) = hit ? 0 : (maxRRPV - 1); }

  // Victim among all ways except `exclude` (pass -1 to exclude nothing); ages the searched ways so the victim reaches maxRRPV
  long victim(long exclude = -1)
  {
    long best = -1;
    for (long way = 0; way < static_cast<long>(std::size(rrpv)); ++way) {
      if (way == exclude)
        continue;
      if (best < 0 || get(way) > get(best))
        best = way;
    }
    if (best < 0)
      return best;

    auto diff = maxRRPV - get(best);
    for (long way = 0; way < static_cast<long>(std::size(rrpv)); ++way) {
      if (way != exclude)
        get(way) += diff;
    }
    return best;
  }

  // Next attempt after `rejected` was given a second chance
  long second_victim(long rejected)
  {
    get(rejected) = maxRRPV - 1;
    return victim(rejected);
  }
};
} // namespace sf

#endif
