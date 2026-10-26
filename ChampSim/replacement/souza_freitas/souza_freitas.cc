#include "souza_freitas.h"

#include <cstdlib>
#include <fmt/core.h>
#include <stdexcept>
#include <string>

#include "champsim.h"
#include "coherence.h"
#include "util/to_underlying.h"

namespace
{
// Read an environment variable once; unset means the default, an unrecognized value is a configuration error
template <typename Parser, typename T>
T read_env(const char* var, Parser parse, T fallback)
{
  const char* raw = std::getenv(var);
  if (raw == nullptr || *raw == '\0')
    return fallback;
  if (auto parsed = parse(raw); parsed.has_value())
    return *parsed;
  throw std::invalid_argument(std::string{"souza_freitas: unrecognized value for "} + var + ": '" + raw + "'");
}
} // namespace

souza_freitas::souza_freitas(CACHE* cache) : souza_freitas(cache, cache->NUM_SET, cache->NUM_WAY) {}

souza_freitas::souza_freitas(CACHE* cache, long sets, long ways)
    : replacement(cache), NUM_SET(sets), NUM_WAY(ways), mode(read_env("SF_MODE", sf::parse_mode, sf::mode::state)),
      base(read_env("SF_BASE", sf::parse_base, sf::base_policy::lru)), rule(read_env("SF_RULE", sf::parse_rule, sf::rule::text)),
      history(static_cast<std::size_t>(sets), sf::eviction_history{static_cast<std::size_t>(2 * ways)})
{
  if (base == sf::base_policy::lru)
    lru_sets.assign(static_cast<std::size_t>(sets), sf::lru_set{ways});
  else
    srrip_sets.assign(static_cast<std::size_t>(sets), sf::srrip_set{ways});
}

uint64_t souza_freitas::block_tag(champsim::address addr) const
{
  return addr.to<uint64_t>() >> champsim::to_underlying(intern_->OFFSET_BITS);
}

sf::candidate_info souza_freitas::candidate(const champsim::cache_block& block) const
{
  // Oracle sharer information: a coherence point holds the mask itself; a private cache asks its directory
  uint64_t sharers = 0;
  if (intern_->is_coherence_point())
    sharers = block.sharers;
  else if (intern_->coherence_home != nullptr)
    sharers = intern_->coherence_home->sharers_of(block.address);

  return {static_cast<sf::mesi>(champsim::to_underlying(block.state)), sf::above_threshold(sharers, NUM_CPUS)};
}

long souza_freitas::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                                champsim::address full_addr, access_type type)
{
  const auto set_idx = static_cast<std::size_t>(set);
  auto first = [&]() {
    return base == sf::base_policy::lru ? lru_sets.at(set_idx).victim() : srrip_sets.at(set_idx).victim();
  };
  auto second = [&](long rejected) {
    return base == sf::base_policy::lru ? lru_sets.at(set_idx).victim(rejected) : srrip_sets.at(set_idx).second_victim(rejected);
  };
  auto info = [&](long way) { return candidate(current_set[way]); };

  auto victim = sf::select_victim(mode, rule, history.at(set_idx), NUM_WAY, first, second, info, stats);
  pending = {true, set, victim, candidate(current_set[victim])};
  return victim;
}

void souza_freitas::replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                           champsim::address victim_addr, access_type type)
{
  // Base policy: identical to lru / srrip fills
  if (base == sf::base_policy::lru)
    lru_sets.at(static_cast<std::size_t>(set)).touch(way, cycle++);
  else
    srrip_sets.at(static_cast<std::size_t>(set)).update(way, false);

  if (mode == sf::mode::off) {
    pending.valid = false;
    return;
  }

  auto& h = history.at(static_cast<std::size_t>(set));

  // A refilled block's history entry is removed
  if (h.remove(block_tag(full_addr)))
    ++stats.refill_removals;

  // Record the block this fill evicted
  if (pending.valid && pending.set == set && pending.way == way) {
    if (h.insert(block_tag(victim_addr), pending.info.state, pending.info.above))
      ++stats.history_age_evictions;
  }
  pending.valid = false;
}

void souza_freitas::update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                             champsim::address victim_addr, access_type type, uint8_t hit)
{
  const auto set_idx = static_cast<std::size_t>(set);
  const bool writeback = access_type{type} == access_type::WRITE;

  // Base policy: identical to lru (skips writeback hits) / srrip (hits only); misses carry way == NUM_WAY and are not indexed
  if (hit) {
    if (base == sf::base_policy::lru) {
      if (!writeback)
        lru_sets.at(set_idx).touch(way, cycle++);
    } else {
      srrip_sets.at(set_idx).update(way, true);
    }
  }

  // Rewards on every non-writeback access, hit or miss
  if (mode != sf::mode::off && !writeback)
    sf::observe_access(history.at(set_idx), block_tag(full_addr), stats);
}

void souza_freitas::replacement_final_stats()
{
  fmt::print("{} souza_freitas mode: {} base: {} rule: {} history_capacity: {} threshold: >{} of {} cores\n", intern_->NAME, sf::name(mode), sf::name(base),
             sf::name(rule), 2 * NUM_WAY, NUM_CPUS / 2, NUM_CPUS);
  fmt::print("{} souza_freitas decisions: {} first_accepted: {} second_chances: {} empty_history_decisions: {}\n", intern_->NAME, stats.decisions,
             stats.first_accepted, stats.second_chances, stats.empty_history_decisions);
  fmt::print("{} souza_freitas rewards: -1 (history hits): {} +1: {} refill_removals: {} history_age_evictions: {}\n", intern_->NAME, stats.history_hits,
             stats.positive_rewards, stats.refill_removals, stats.history_age_evictions);

  auto n = static_cast<double>(stats.weighted_decisions);
  auto avg = [n](double sum) { return n > 0 ? sum / n : 0.0; };
  fmt::print("{} souza_freitas avg_weights_at_decision (n={}): wM: {:.4f} wE: {:.4f} wS: {:.4f} wAT: {:.4f} wBT: {:.4f}\n", intern_->NAME,
             stats.weighted_decisions, avg(stats.sum_m), avg(stats.sum_e), avg(stats.sum_s), avg(stats.sum_at), avg(stats.sum_bt));

  // Final weights averaged over sets with a non-empty history
  double m = 0, e = 0, s = 0, at = 0, bt = 0;
  std::size_t nonempty = 0;
  for (const auto& h : history) {
    if (h.empty())
      continue;
    auto w = sf::compute_weights(h);
    m += w.m;
    e += w.e;
    s += w.s;
    at += w.at;
    bt += w.bt;
    ++nonempty;
  }
  auto k = static_cast<double>(nonempty);
  auto favg = [k](double sum) { return k > 0 ? sum / k : 0.0; };
  fmt::print("{} souza_freitas final_avg_weights (sets={}): wM: {:.4f} wE: {:.4f} wS: {:.4f} wAT: {:.4f} wBT: {:.4f}\n", intern_->NAME, nonempty, favg(m),
             favg(e), favg(s), favg(at), favg(bt));
}
