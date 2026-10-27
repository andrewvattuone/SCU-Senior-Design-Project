#ifndef REPLACEMENT_RL_MESI_DUEL_H
#define REPLACEMENT_RL_MESI_DUEL_H

/*
 * C2: per-cache set dueling between our RL policy and BRRIP (DIP, Qureshi et al. ISCA 2007; DRRIP, Jaleel et al. ISCA 2010).
 *
 * A few leader sets always use RL, a few always use BRRIP, and a saturating counter PSEL counts their misses. Every other
 * (follower) set uses whichever side PSEL currently favors. Both policies' metadata (RL's LRU stamps in rl_core.h, BRRIP's
 * RRPVs here) are kept up to date in every set, so a follower can switch sides at any access.
 *
 * Constants, leader placement and PSEL semantics match replacement/drrip, with RL in the place of SRRIP. ChampSim-independent
 * and header-only so it can be unit tested without building a cache.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace rlm
{
// replacement/drrip/drrip.h: maxRRPV = 3 (2-bit RRPV), BRRIP_MAX = 32, PSEL_WIDTH = 10
inline constexpr unsigned BRRIP_MAX_RRPV = 3;
inline constexpr unsigned BRRIP_LONG_PERIOD = 32;
inline constexpr unsigned DUEL_PSEL_BITS = 10;

/*
 * BRRIP's "small probability" of a long re-reference insertion, done as drrip does it: a counter shared by the whole cache makes
 * every BRRIP_LONG_PERIOD-th insertion long. Deterministic, so tests can predict it.
 */
class bimodal_throttle
{
  unsigned period_;
  unsigned count_ = 0;

public:
  explicit bimodal_throttle(unsigned period = BRRIP_LONG_PERIOD) : period_(std::max(period, 1u)) {}

  // True when this insertion should be long (maxRRPV - 1) instead of distant (maxRRPV)
  bool next_is_long()
  {
    if (++count_ < period_)
      return false;
    count_ = 0;
    return true;
  }
};

// RRPVs of one set with SRRIP's victim search. RRPVs start at 0, as in drrip.
class brrip_set
{
  std::vector<unsigned> rrpv_;

public:
  explicit brrip_set(long ways) : rrpv_(static_cast<std::size_t>(std::max(ways, 1L)), 0) {}

  [[nodiscard]] long ways() const { return static_cast<long>(std::size(rrpv_)); }
  [[nodiscard]] unsigned rrpv(long way) const { return rrpv_.at(static_cast<std::size_t>(way)); }

  // A demand fill: distant re-reference, or long when the throttle says so
  void on_fill(long way, bool long_rereference) { rrpv_.at(static_cast<std::size_t>(way)) = long_rereference ? BRRIP_MAX_RRPV - 1 : BRRIP_MAX_RRPV; }
  void on_hit(long way) { rrpv_.at(static_cast<std::size_t>(way)) = 0; }
  // Writeback fills and writeback hits are neither promoted nor throttled (drrip sets maxRRPV - 1 for both)
  void on_writeback(long way) { rrpv_.at(static_cast<std::size_t>(way)) = BRRIP_MAX_RRPV - 1; }

  // First way at the highest RRPV, without aging
  [[nodiscard]] long peek_victim() const { return static_cast<long>(std::distance(std::begin(rrpv_), std::max_element(std::begin(rrpv_), std::end(rrpv_)))); }

  /*
   * Age every way until one reaches maxRRPV and return the first such way (drrip::find_victim). Calling it again before the
   * fill changes nothing, so a retried fill sees the same victim.
   */
  long victim()
  {
    auto v = peek_victim();
    auto delta = BRRIP_MAX_RRPV - rrpv_.at(static_cast<std::size_t>(v));
    for (auto& r : rrpv_)
      r += delta;
    return v;
  }
};

// BRRIP state for a whole cache: one brrip_set per set and the shared insertion throttle
class brrip_policy
{
  std::vector<brrip_set> sets_;
  bimodal_throttle throttle_;

public:
  brrip_policy(long num_sets, long ways, unsigned long_period = BRRIP_LONG_PERIOD)
      : sets_(static_cast<std::size_t>(std::max(num_sets, 1L)), brrip_set{ways}), throttle_(long_period)
  {
  }

  [[nodiscard]] const brrip_set& set(long s) const { return sets_.at(static_cast<std::size_t>(s)); }

  void on_fill(long s, long way) { sets_.at(static_cast<std::size_t>(s)).on_fill(way, throttle_.next_is_long()); }
  void on_hit(long s, long way) { sets_.at(static_cast<std::size_t>(s)).on_hit(way); }
  void on_writeback(long s, long way) { sets_.at(static_cast<std::size_t>(s)).on_writeback(way); }
  long victim(long s) { return sets_.at(static_cast<std::size_t>(s)).victim(); }
};

enum class set_role : uint8_t { follower, leader_rl, leader_brrip };

struct duel_stats {
  uint64_t misses_leader_rl = 0;
  uint64_t misses_leader_brrip = 0;
  uint64_t misses_follower_rl = 0;    // follower misses while PSEL favored RL
  uint64_t misses_follower_brrip = 0; // follower misses while PSEL favored BRRIP
  uint64_t preference_flips = 0;      // times the followers' side changed
  uint64_t psel_samples = 0;          // PSEL after every miss: count, sum, extremes
  uint64_t psel_sum = 0;
  unsigned psel_min = 0;
  unsigned psel_max = 0;

  [[nodiscard]] double psel_mean() const { return psel_samples == 0 ? 0.0 : static_cast<double>(psel_sum) / static_cast<double>(psel_samples); }
};

/*
 * Leader placement (drrip's categorizer in inc/msl/stat_methods.h): the cache is cut into `leaders` constituencies of `stride`
 * sets; in constituency i the BRRIP leader is at offset i % stride and the RL leader at offset (i + 1) % stride. For
 * power-of-two caches with the default leader count this picks exactly drrip's BRRIP and SRRIP leaders.
 *
 * PSEL (also as drrip): starts at 0, a miss in an RL leader adds 1, a miss in a BRRIP leader subtracts 1, saturating at 0 and
 * 2^bits - 1. Followers use BRRIP when PSEL >= (2^bits - 1) / 2, so they start with RL.
 */
class set_dueler
{
  std::vector<set_role> roles_;
  long leaders_;
  unsigned psel_ = 0;
  unsigned psel_limit_;
  unsigned threshold_;

  void sample()
  {
    stats.psel_min = stats.psel_samples == 0 ? psel_ : std::min(stats.psel_min, psel_);
    stats.psel_max = stats.psel_samples == 0 ? psel_ : std::max(stats.psel_max, psel_);
    ++stats.psel_samples;
    stats.psel_sum += psel_;
  }

public:
  duel_stats stats;

  // msl::get_sample_rate, extended below 8 sets (where drrip asserts) to keep one leader per side whenever there are 2+ sets
  [[nodiscard]] static long default_leaders(long num_sets)
  {
    if (num_sets < 2)
      return 0;
    long rate = num_sets >= 1024 ? 32 : num_sets >= 256 ? 16 : num_sets >= 64 ? 8 : 4;
    return std::max(num_sets / rate, 1L);
  }

  set_dueler(long num_sets, long leaders_per_policy, unsigned psel_bits = DUEL_PSEL_BITS)
  {
    if (num_sets < 1)
      throw std::invalid_argument("set_dueler: num_sets must be >= 1");
    if (leaders_per_policy < 0)
      throw std::invalid_argument("set_dueler: leaders_per_policy must be >= 0");
    if (psel_bits < 1 || psel_bits > 31)
      throw std::invalid_argument("set_dueler: psel_bits must be in [1, 31]");

    // A cache too small for the request gets as many leader pairs as fit; a 1-set cache has none and always uses RL
    leaders_ = std::min(leaders_per_policy, num_sets / 2);
    psel_limit_ = (1u << psel_bits) - 1;
    threshold_ = psel_limit_ / 2;

    roles_.assign(static_cast<std::size_t>(num_sets), set_role::follower);
    if (leaders_ > 0) {
      long stride = num_sets / leaders_; // >= 2, so the two leaders of a constituency differ
      for (long i = 0; i < leaders_; ++i) {
        roles_.at(static_cast<std::size_t>(i * stride + i % stride)) = set_role::leader_brrip;
        roles_.at(static_cast<std::size_t>(i * stride + (i + 1) % stride)) = set_role::leader_rl;
      }
    }
  }

  explicit set_dueler(long num_sets) : set_dueler(num_sets, default_leaders(num_sets)) {}

  [[nodiscard]] long num_sets() const { return static_cast<long>(std::size(roles_)); }
  [[nodiscard]] long leaders_per_policy() const { return leaders_; }
  [[nodiscard]] unsigned psel() const { return psel_; }
  [[nodiscard]] unsigned psel_limit() const { return psel_limit_; }
  [[nodiscard]] unsigned threshold() const { return threshold_; }

  [[nodiscard]] set_role role(long set) const { return roles_.at(static_cast<std::size_t>(set)); }
  [[nodiscard]] bool followers_use_rl() const { return psel_ < threshold_; }

  // Which policy picks the victim in this set right now
  [[nodiscard]] bool use_rl(long set) const
  {
    switch (role(set)) {
    case set_role::leader_rl:
      return true;
    case set_role::leader_brrip:
      return false;
    case set_role::follower:
    default:
      return followers_use_rl();
    }
  }

  // A demand miss (not a writeback) in `set`: leaders move PSEL toward the other side; followers are only counted
  set_role on_miss(long set)
  {
    auto r = role(set);
    bool before = followers_use_rl();
    switch (r) {
    case set_role::leader_rl:
      ++stats.misses_leader_rl;
      psel_ = std::min(psel_ + 1, psel_limit_);
      break;
    case set_role::leader_brrip:
      ++stats.misses_leader_brrip;
      psel_ = psel_ == 0 ? 0 : psel_ - 1;
      break;
    case set_role::follower:
      ++(before ? stats.misses_follower_rl : stats.misses_follower_brrip);
      break;
    }
    if (before != followers_use_rl())
      ++stats.preference_flips;
    sample();
    return r;
  }
};
} // namespace rlm

#endif
