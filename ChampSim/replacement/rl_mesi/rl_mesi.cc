#include "rl_mesi.h"

#include <cstdlib>
#include <fmt/core.h>
#include <optional>
#include <stdexcept>
#include <string>

#include "champsim.h"
#include "coherence.h"
#include "util/to_underlying.h"

namespace
{
[[noreturn]] void bad_value(const char* var, const char* raw)
{
  throw std::invalid_argument(std::string{"rl_mesi: unrecognized value for "} + var + ": '" + raw + "'");
}

// Read an environment variable once; unset means the default, an unrecognized value is a configuration error
template <typename Parser, typename T>
T read_env(const char* var, Parser parse, T fallback)
{
  const char* raw = std::getenv(var);
  if (raw == nullptr || *raw == '\0')
    return fallback;
  if (std::optional<T> parsed = parse(raw); parsed.has_value())
    return *parsed;
  bad_value(var, raw);
}

std::optional<long> parse_long(const char* s)
{
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  return (*end == '\0' && v > 0) ? std::optional<long>{v} : std::nullopt;
}

std::optional<double> parse_fraction(const char* s)
{
  char* end = nullptr;
  double v = std::strtod(s, &end);
  return (*end == '\0' && v >= 0.0 && v <= 1.0) ? std::optional<double>{v} : std::nullopt;
}

std::optional<bool> parse_flag(const char* s)
{
  std::string v{s};
  if (v == "1")
    return true;
  if (v == "0")
    return false;
  return std::nullopt;
}

rlm::config config_from_env()
{
  rlm::config cfg;
  cfg.m = read_env("RL_MODE", [](const char* s) { return rlm::parse_mode(s); }, cfg.m);
  cfg.window = read_env("RL_WINDOW", parse_long, cfg.window);
  cfg.alpha = read_env("RL_ALPHA", parse_fraction, cfg.alpha);
  cfg.epsilon = read_env("RL_EPSILON", parse_fraction, cfg.epsilon);
  cfg.protect_feedback = read_env("RL_PROTECT", parse_flag, cfg.protect_feedback);
  cfg.istate = read_env("RL_ISTATE", parse_flag, cfg.istate);
  return cfg;
}

rlm::sharer_source sharer_source_from_env()
{
  return read_env("RL_SHARERS", [](const char* s) { return rlm::parse_sharer_source(s); }, rlm::sharer_source::delivered);
}

rlm::threshold_config threshold_config_from_env()
{
  rlm::threshold_config cfg;
  cfg.m = read_env("RL_THRESHOLD", [](const char* s) { return rlm::parse_threshold_mode(s); }, cfg.m);
  return cfg;
}

// With RL_MODE=off the policy must be exactly LRU, so dueling against BRRIP is disabled too
bool duel_from_env(const rlm::config& cfg) { return read_env("RL_DUEL", parse_flag, true) && cfg.m != rlm::mode::off; }
} // namespace

rl_mesi::rl_mesi(CACHE* cache) : rl_mesi(cache, cache->NUM_SET, cache->NUM_WAY) {}

rl_mesi::rl_mesi(CACHE* cache, long sets_, long ways_)
    : replacement(cache), NUM_SET(sets_), NUM_WAY(ways_), learner(config_from_env()), sharer_src(sharer_source_from_env()),
      threshold(static_cast<std::size_t>(sets_), NUM_CPUS, threshold_config_from_env()), duel_enabled(duel_from_env(learner.cfg())), dueler(sets_),
      brrip(sets_, ways_), sets(static_cast<std::size_t>(sets_), rlm::set_state{ways_})
{
}

uint64_t rl_mesi::block_tag(champsim::address addr) const { return addr.to<uint64_t>() >> champsim::to_underlying(intern_->OFFSET_BITS); }

std::size_t rl_mesi::sharer_count_of(const champsim::cache_block& block) const
{
  // A coherence point holds the directory's exact mask itself, which real hardware has there too
  if (intern_->is_coherence_point())
    return rlm::count_sharers(block.sharers);

  // A private cache outside any coherence domain has no sharer information
  if (intern_->coherence_home == nullptr)
    return 0;

  // C3: the count the directory delivered with this copy's grant (may be stale) vs. the directory's current mask (oracle)
  if (sharer_src == rlm::sharer_source::delivered)
    return block.delivered_sharers;
  return rlm::count_sharers(intern_->coherence_home->sharers_of(block.address));
}

rlm::features rl_mesi::features_of(long set, const champsim::cache_block& block, const rlm::line_meta& meta) const
{
  auto state = static_cast<rlm::mesi>(champsim::to_underlying(block.state));
  auto state_feature = rlm::to_state_feature(state, meta.previously_invalidated, learner.cfg().istate);
  const auto set_idx = static_cast<std::size_t>(set);

  if (!intern_->is_coherence_point() && intern_->coherence_home != nullptr) {
    const bool delivered_above = threshold.heavy(set_idx, block.delivered_sharers);
    const bool oracle_above = threshold.heavy(set_idx, static_cast<uint32_t>(rlm::count_sharers(intern_->coherence_home->sharers_of(block.address))));
    ++agreement.evaluations;
    if (delivered_above && !oracle_above)
      ++agreement.delivered_above_oracle_below;
    if (!delivered_above && oracle_above)
      ++agreement.delivered_below_oracle_above;
  }

  // C1: heavy or light under the threshold currently in force for this set
  return {state_feature, threshold.heavy(set_idx, static_cast<uint32_t>(sharer_count_of(block)))};
}

// replacement_cache_fill runs before the cache installs the block, so the filled block's count is read on the next call
void rl_mesi::observe_last_fill()
{
  if (!last_fill.valid)
    return;
  last_fill.valid = false;
  const auto& block = intern_->block.at(static_cast<std::size_t>(last_fill.set * NUM_WAY + last_fill.way));
  if (block.valid && block_tag(block.address) == last_fill.tag)
    threshold.observe(static_cast<uint32_t>(sharer_count_of(block)));
}

long rl_mesi::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                          champsim::address full_addr, access_type type)
{
  observe_last_fill();
  auto& s = sets.at(static_cast<std::size_t>(set));
  auto info = [&](long way) { return features_of(set, current_set[way], s.lines.at(static_cast<std::size_t>(way))); };

  // C2: a set run by BRRIP (a BRRIP leader, or a follower while BRRIP is winning) takes BRRIP's victim; RL still learns from it
  if (duel_enabled && !dueler.use_rl(set)) {
    auto victim = brrip.victim(set);
    pending = {true, set, rlm::decision{victim, false, false, {}}, info(victim), true};
    return victim;
  }

  auto d = learner.choose_victim(s, info);
  pending = {true, set, d, info(d.victim), false};
  return d.victim;
}

void rl_mesi::replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                     champsim::address victim_addr, access_type type)
{
  observe_last_fill();
  auto& s = sets.at(static_cast<std::size_t>(set));
  const bool writeback = access_type{type} == access_type::WRITE;

  // A fill into a full set always follows find_victim for the same set and way; anything else filled an empty way
  std::optional<rlm::history_entry> evicted;
  if (pending.valid && pending.set == set && pending.d.victim == way) {
    if (pending.by_brrip)
      ++brrip_victims;
    learner.commit(s, pending.d);
    evicted = rlm::history_entry{block_tag(victim_addr), pending.victim_features};
  }
  pending.valid = false;

  // Both policies' metadata is kept in every set, so a set can switch sides at any time
  if (duel_enabled) {
    if (writeback) {
      brrip.on_writeback(set, way);
    } else {
      dueler.on_miss(set);
      brrip.on_fill(set, way);
    }
  }

  learner.on_fill(s, way, block_tag(full_addr), evicted);
  last_fill = {true, set, way, block_tag(full_addr)};
}

void rl_mesi::update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                       champsim::address victim_addr, access_type type, uint8_t hit)
{
  observe_last_fill();
  const bool writeback = access_type{type} == access_type::WRITE;

  // Misses carry way == NUM_WAY; nothing below indexes a line on a miss
  if (hit && duel_enabled) {
    if (writeback)
      brrip.on_writeback(set, way);
    else
      brrip.on_hit(set, way);
  }
  if (!writeback && learner.cfg().m != rlm::mode::off)
    threshold.on_access(static_cast<std::size_t>(set), hit);

  learner.on_access(sets.at(static_cast<std::size_t>(set)), way, block_tag(full_addr), hit, writeback);
}

void rl_mesi::replacement_final_stats()
{
  const auto& cfg = learner.cfg();
  const auto& st = learner.stats;
  const auto& w = learner.weights();

  fmt::print("{} rl_mesi mode: {} window: {} alpha: {} epsilon: {} protect_feedback: {} istate: {} sharers: {} threshold: {} duel: {}\n", intern_->NAME,
             rlm::name(cfg.m), cfg.window, cfg.alpha, cfg.epsilon, cfg.protect_feedback, cfg.istate, rlm::name(sharer_src), rlm::name(threshold.mode()),
             duel_enabled);
  fmt::print("{} rl_mesi decisions: {} chose_oldest: {} chose_other: {} explorations: {} brrip_victims: {}\n", intern_->NAME, st.decisions,
             st.chose_oldest, st.chose_other, st.explorations, brrip_victims);
  fmt::print("{} rl_mesi rewards: evict_bad(-1): {} evict_good(+1): {} protect_good(-1): {} protect_wasted(+1): {}\n", intern_->NAME, st.evict_bad,
             st.evict_good, st.protect_good, st.protect_wasted);
  fmt::print("{} rl_mesi coherence: invalidations_seen: {} iprev_fills: {}\n", intern_->NAME, st.invalidations_seen, st.iprev_fills);
  fmt::print("{} rl_mesi sharer_count: evaluations: {} delivered_above_oracle_below: {} delivered_below_oracle_above: {}\n", intern_->NAME,
             agreement.evaluations, agreement.delivered_above_oracle_below, agreement.delivered_below_oracle_above);
  fmt::print("{} rl_mesi weights: wS: {:.4f} wE: {:.4f} wM: {:.4f} wIprev: {:.4f} wBelow: {:.4f} wAbove: {:.4f}\n", intern_->NAME,
             w.state_weight(rlm::state_feature::S), w.state_weight(rlm::state_feature::E), w.state_weight(rlm::state_feature::M),
             w.state_weight(rlm::state_feature::Iprev), w.share_weight(false), w.share_weight(true));

  fmt::print("{} rl_mesi threshold: mode: {} current: >{} paper: >{} epochs: {} average: {:.3f}\n", intern_->NAME, rlm::name(threshold.mode()),
             threshold.winner_threshold(), threshold.paper_threshold(), threshold.epochs(), threshold.average());
  for (const auto& c : threshold.candidates())
    fmt::print("{} rl_mesi threshold_candidate: >{} leader_sets: {} wins: {} misses: {} accesses: {}\n", intern_->NAME, c.threshold, c.leader_sets, c.wins,
               c.total_misses, c.total_accesses);

  const auto& ds = dueler.stats;
  fmt::print("{} rl_mesi duel: enabled: {} psel: {} of {} followers_use_rl: {} leaders_per_policy: {} psel_mean: {:.1f} preference_flips: {}\n",
             intern_->NAME, duel_enabled, dueler.psel(), dueler.psel_limit(), dueler.followers_use_rl(), dueler.leaders_per_policy(), ds.psel_mean(),
             ds.preference_flips);
  fmt::print("{} rl_mesi duel_misses: leader_rl: {} leader_brrip: {} follower_rl: {} follower_brrip: {}\n", intern_->NAME, ds.misses_leader_rl,
             ds.misses_leader_brrip, ds.misses_follower_rl, ds.misses_follower_brrip);
}
