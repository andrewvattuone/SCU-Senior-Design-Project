#ifndef REPLACEMENT_RL_MESI_CORE_H
#define REPLACEMENT_RL_MESI_CORE_H

/*
 * ChampSim-independent core of our coherence-aware RL replacement policy (improved version of Souza & Freitas, IEEE
 * Access 2024; the faithful reproduction lives in replacement/souza_freitas).
 *
 * Differences from the paper implemented here:
 *   C4  Weights are learned from rewards (w += alpha * (r - w)) instead of being history ratios; "keep" decisions also
 *       get feedback; the victim is the highest-scoring of the N least recently used ways instead of a single second chance.
 *   C5  A fourth state, Iprev, marks a block whose previous copy in this cache was invalidated by another core. Misses caused
 *       by invalidations are never counted as bad evictions.
 *   C3  In a private cache the sharer count is, by default, the count the directory delivered with the grant (possibly stale),
 *       not the directory's current mask (sharer_source).
 *
 * Everything here is pure and header-only so that it can be unit tested without building a cache.
 */

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace rlm
{
enum class mode { off, state, bitset, both };

// Order matters: the first four index the state weights
enum class state_feature : uint8_t { S = 0, E, M, Iprev, none };
inline constexpr std::size_t NUM_STATE_FEATURES = 4;

// Mirrors the numeric values of champsim::coherence_state
enum class mesi : uint8_t { I = 0, S, E, M };

struct features {
  state_feature state = state_feature::none;
  bool above = false;
};

struct config {
  mode m = mode::both;
  long window = 4;
  double alpha = 0.1;
  double epsilon = 0.0;
  bool protect_feedback = true;
  bool istate = true;
};

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

inline constexpr std::string_view name(mode m)
{
  constexpr std::string_view names[] = {"off", "state", "bitset", "both"};
  return names[static_cast<int>(m)];
}

// Where the sharer count comes from (C3). `delivered` is what real hardware can know: a private cache keeps the count the
// directory reported when its copy was granted, which goes stale as other cores join or leave. `oracle` reads the directory's
// current, exact sharer mask at every decision. At the coherence point both use the directory's own (exact) mask.
enum class sharer_source { delivered, oracle };

inline std::optional<sharer_source> parse_sharer_source(std::string_view s)
{
  if (s == "delivered")
    return sharer_source::delivered;
  if (s == "oracle")
    return sharer_source::oracle;
  return std::nullopt;
}

inline constexpr std::string_view name(sharer_source src) { return src == sharer_source::delivered ? "delivered" : "oracle"; }

// A block is above threshold if MORE than half of the cores share it (the paper's fixed threshold; C1 replaces this later)
inline bool above_threshold(std::size_t sharer_count, std::size_t num_cpus) { return sharer_count > num_cpus / 2; }

// Same test on a sharer bitmask (one bit per core)
inline std::size_t count_sharers(uint64_t sharer_mask) { return std::bitset<64>{sharer_mask}.count(); }
inline bool above_threshold_mask(uint64_t sharer_mask, std::size_t num_cpus) { return above_threshold(count_sharers(sharer_mask), num_cpus); }

// Valid lines carry S/E/M; a non-coherent cache reports I for valid lines, which carries no state information
inline state_feature to_state_feature(mesi state, bool previously_invalidated, bool istate_enabled)
{
  if (istate_enabled && previously_invalidated)
    return state_feature::Iprev;
  switch (state) {
  case mesi::S:
    return state_feature::S;
  case mesi::E:
    return state_feature::E;
  case mesi::M:
    return state_feature::M;
  case mesi::I:
  default:
    return state_feature::none;
  }
}

struct statistics {
  uint64_t decisions = 0;
  uint64_t chose_oldest = 0;
  uint64_t chose_other = 0;
  uint64_t explorations = 0;
  uint64_t evict_bad = 0;        // evicted block missed again: -1
  uint64_t evict_good = 0;       // evicted block aged out of the history: +1
  uint64_t protect_good = 0;     // protected block was hit: -1 (blocks like it should not be evicted)
  uint64_t protect_wasted = 0;   // protected block was evicted without a hit: +1
  uint64_t invalidations_seen = 0;
  uint64_t iprev_fills = 0;
};

/*
 * One learned weight per feature value: "how good is it to evict a block with this feature?" Each reward moves the
 * weights of the features the block had toward r with an exponential moving average, so every weight stays in [-1, 1].
 */
class weight_table
{
  std::array<double, NUM_STATE_FEATURES> w_state_{};
  std::array<double, 2> w_share_{};

  static bool uses_state(mode m) { return m == mode::state || m == mode::both; }
  static bool uses_share(mode m) { return m == mode::bitset || m == mode::both; }

public:
  [[nodiscard]] double score(mode m, features f) const
  {
    double v = 0;
    if (uses_state(m) && f.state != state_feature::none)
      v += w_state_.at(static_cast<std::size_t>(f.state));
    if (uses_share(m))
      v += w_share_.at(f.above ? 1 : 0);
    return v;
  }

  void reward(mode m, features f, double r, double alpha)
  {
    if (uses_state(m) && f.state != state_feature::none) {
      auto& w = w_state_.at(static_cast<std::size_t>(f.state));
      w += alpha * (r - w);
    }
    if (uses_share(m)) {
      auto& w = w_share_.at(f.above ? 1 : 0);
      w += alpha * (r - w);
    }
  }

  [[nodiscard]] double state_weight(state_feature s) const { return w_state_.at(static_cast<std::size_t>(s)); }
  [[nodiscard]] double share_weight(bool above) const { return w_share_.at(above ? 1 : 0); }
};

struct history_entry {
  uint64_t tag;
  features f;
};

/*
 * Per-set eviction history (kept from the paper): 2 x ways entries, front() is the oldest. An entry that ages out without its
 * block returning was a good eviction.
 */
class eviction_history
{
  std::size_t capacity_;
  std::vector<history_entry> entries_;

public:
  explicit eviction_history(std::size_t capacity) : capacity_(capacity) { entries_.reserve(capacity); }

  [[nodiscard]] std::size_t size() const { return std::size(entries_); }

  std::optional<history_entry> remove(uint64_t tag)
  {
    auto it = std::find_if(std::begin(entries_), std::end(entries_), [tag](const auto& e) { return e.tag == tag; });
    if (it == std::end(entries_))
      return std::nullopt;
    auto found = *it;
    entries_.erase(it);
    return found;
  }

  // Record an evicted block; returns the entry that aged out to make room, if any
  std::optional<history_entry> insert(uint64_t tag, features f)
  {
    remove(tag);
    if (capacity_ == 0)
      return std::nullopt;
    std::optional<history_entry> aged_out;
    if (std::size(entries_) >= capacity_) {
      aged_out = entries_.front();
      entries_.erase(std::begin(entries_));
    }
    entries_.push_back({tag, f});
    return aged_out;
  }
};

// The policy's own view of one way: which block it last installed there, and that block's learning metadata
struct line_meta {
  bool resident = false; // false once the policy knows the block left (eviction, or detected invalidation)
  uint64_t tag = 0;
  bool previously_invalidated = false;
  bool is_protected = false;
  features protect_snapshot{};
  uint64_t last_used = 0;
};

/*
 * Per-set state. `invalidated` remembers tags whose copies were invalidated and whose way has since been reused, so a returning
 * block can still be recognized (bounded to `ways` entries, oldest dropped first).
 */
struct set_state {
  std::vector<line_meta> lines;
  eviction_history history;
  std::vector<uint64_t> invalidated;

  explicit set_state(long ways) : lines(static_cast<std::size_t>(ways)), history(static_cast<std::size_t>(2 * ways)) {}

  void remember_invalidated(uint64_t tag)
  {
    if (std::size(invalidated) >= std::size(lines))
      invalidated.erase(std::begin(invalidated));
    invalidated.push_back(tag);
  }

  bool forget_invalidated(uint64_t tag)
  {
    auto it = std::find(std::begin(invalidated), std::end(invalidated), tag);
    if (it == std::end(invalidated))
      return false;
    invalidated.erase(it);
    return true;
  }

  // Ways ordered least recently used first; ties go to the lower way, exactly as replacement/lru
  [[nodiscard]] std::vector<long> lru_order() const
  {
    std::vector<long> order(std::size(lines));
    for (std::size_t i = 0; i < std::size(order); ++i)
      order[i] = static_cast<long>(i);
    std::stable_sort(std::begin(order), std::end(order), [this](long a, long b) {
      return lines.at(static_cast<std::size_t>(a)).last_used < lines.at(static_cast<std::size_t>(b)).last_used;
    });
    return order;
  }
};

// Outcome of choose_victim: the victim and the older ways it was preferred over (with their features at decision time)
struct decision {
  long victim = 0;
  bool learned = false;  // false when the policy is off: the victim is plain LRU and nothing is recorded
  bool explored = false;
  std::vector<std::pair<long, features>> passed_over;
};

/*
 * The learning policy for one cache. `info(way)` returns the current features of the block in a way; it is supplied by the
 * ChampSim wrapper because the coherence state and sharer count live in the cache and its directory.
 */
class policy
{
  config cfg_;
  weight_table weights_;
  std::mt19937_64 rng_{0x5eed};
  uint64_t stamp_ = 0;

public:
  statistics stats;

  explicit policy(config cfg, weight_table initial = {}) : cfg_(cfg), weights_(initial) {}

  [[nodiscard]] const config& cfg() const { return cfg_; }
  [[nodiscard]] const weight_table& weights() const { return weights_; }

  /*
   * Pick a victim without changing any state. ChampSim may ask for a victim and then retry the fill later (for example when the
   * lower level's write queue is full), so the effects of a decision are applied by commit() only once the fill really happens.
   */
  template <typename Info>
  decision choose_victim(const set_state& s, Info&& info)
  {
    auto order = s.lru_order();
    decision d{order.front(), false, false, {}};
    if (cfg_.m == mode::off || std::size(order) < 2)
      return d;

    d.learned = true;
    auto window = static_cast<std::size_t>(std::clamp<long>(cfg_.window, 1, static_cast<long>(std::size(order))));

    std::size_t chosen = 0;
    if (cfg_.epsilon > 0 && std::uniform_real_distribution<double>{0.0, 1.0}(rng_) < cfg_.epsilon) {
      d.explored = true;
      chosen = std::uniform_int_distribution<std::size_t>{0, window - 1}(rng_);
    } else {
      // Highest eviction score wins; scanning oldest first with a strict comparison sends ties to the older way
      double best = weights_.score(cfg_.m, info(order.front()));
      for (std::size_t i = 1; i < window; ++i) {
        if (auto v = weights_.score(cfg_.m, info(order[i])); v > best) {
          best = v;
          chosen = i;
        }
      }
    }

    d.victim = order[chosen];
    for (std::size_t i = 0; i < chosen; ++i)
      d.passed_over.push_back({order[i], info(order[i])});
    return d;
  }

  // Apply a decision whose fill has happened: statistics, protection marks, and the reward for a protection that was wasted
  void commit(set_state& s, const decision& d)
  {
    if (!d.learned)
      return;
    ++stats.decisions;
    if (d.explored)
      ++stats.explorations;
    (d.passed_over.empty() ? stats.chose_oldest : stats.chose_other)++;

    // Ways older than the victim were passed over: remember what they looked like so their outcome can be judged
    if (cfg_.protect_feedback) {
      for (const auto& [way, f] : d.passed_over) {
        auto& line = s.lines.at(static_cast<std::size_t>(way));
        line.is_protected = true;
        line.protect_snapshot = f;
      }
    }

    auto& vline = s.lines.at(static_cast<std::size_t>(d.victim));
    if (vline.is_protected) {
      ++stats.protect_wasted;
      weights_.reward(cfg_.m, vline.protect_snapshot, +1.0, cfg_.alpha);
      vline.is_protected = false;
    }
  }

  /*
   * A block was installed in `way`. `evicted` holds the victim's tag and features when this fill replaced a valid block; when it
   * did not, a block the policy still believed resident in that way must have been invalidated by the coherence protocol.
   */
  void on_fill(set_state& s, long way, uint64_t incoming_tag, std::optional<history_entry> evicted)
  {
    auto& line = s.lines.at(static_cast<std::size_t>(way));

    if (cfg_.m != mode::off) {
      if (evicted.has_value()) {
        if (auto aged = s.history.insert(evicted->tag, evicted->f); aged.has_value()) {
          ++stats.evict_good;
          weights_.reward(cfg_.m, aged->f, +1.0, cfg_.alpha);
        }
      } else if (line.resident) {
        ++stats.invalidations_seen;
        s.remember_invalidated(line.tag);
      }
    }

    bool iprev = s.forget_invalidated(incoming_tag);
    for (std::size_t v = 0; v < std::size(s.lines); ++v) {
      auto& other = s.lines[v];
      if (static_cast<long>(v) != way && other.resident && other.tag == incoming_tag) {
        // The block is being filled, so its old copy in way v is gone: it was invalidated
        ++stats.invalidations_seen;
        other.resident = false;
        other.is_protected = false;
        iprev = true;
      }
    }
    if (iprev)
      ++stats.iprev_fills;

    s.history.remove(incoming_tag);
    line = line_meta{};
    line.resident = true;
    line.tag = incoming_tag;
    line.previously_invalidated = iprev;
    line.last_used = stamp_++;
  }

  // Every tag check. Writebacks from the upper level neither update recency (as replacement/lru) nor produce rewards.
  void on_access(set_state& s, long way, uint64_t tag, bool hit, bool writeback)
  {
    if (writeback)
      return;
    if (hit) {
      auto& line = s.lines.at(static_cast<std::size_t>(way));
      line.last_used = stamp_++;
      if (cfg_.m != mode::off && line.is_protected) {
        ++stats.protect_good;
        weights_.reward(cfg_.m, line.protect_snapshot, -1.0, cfg_.alpha);
        line.is_protected = false;
      }
      return;
    }
    if (cfg_.m == mode::off)
      return;
    if (auto entry = s.history.remove(tag); entry.has_value()) {
      ++stats.evict_bad;
      weights_.reward(cfg_.m, entry->f, -1.0, cfg_.alpha);
    }
  }
};
} // namespace rlm

#endif
