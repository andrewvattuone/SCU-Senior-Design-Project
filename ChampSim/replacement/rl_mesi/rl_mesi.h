#ifndef REPLACEMENT_RL_MESI_H
#define REPLACEMENT_RL_MESI_H

#include <cstdint>
#include <vector>

#include "cache.h"
#include "duel.h"
#include "modules.h"
#include "rl_core.h"
#include "threshold.h"

/*
 * Our coherence-aware RL replacement policy (improved version of Souza & Freitas, IEEE Access 2024). LRU orders the ways; among the
 * RL_WINDOW oldest, the block with the highest learned eviction score is evicted. Set dueling against BRRIP (C2) decides per set
 * whether the RL choice or BRRIP's is used, and the sharing threshold (C1) is learned at run time. See README.md for the design.
 * Configured at run time with RL_MODE, RL_WINDOW, RL_ALPHA, RL_EPSILON, RL_PROTECT, RL_ISTATE, RL_SHARERS, RL_THRESHOLD and RL_DUEL.
 */
class rl_mesi : public champsim::modules::replacement
{
  long NUM_SET, NUM_WAY;
  rlm::policy learner;
  rlm::sharer_source sharer_src;
  rlm::sharing_threshold threshold;
  bool duel_enabled;
  rlm::set_dueler dueler;
  rlm::brrip_policy brrip;

  // C3 diagnostics (private caches only): how often the delivered and the oracle sharer counts fall on different sides of the
  // threshold, counted per feature evaluation whichever source is in use
  struct threshold_agreement {
    uint64_t evaluations = 0;
    uint64_t delivered_above_oracle_below = 0; // stale high: sharers have left since the grant
    uint64_t delivered_below_oracle_above = 0; // stale low: sharers have joined since the grant
  };
  mutable threshold_agreement agreement{};
  std::vector<rlm::set_state> sets;

  // Decision made by find_victim; replacement_cache_fill commits it and records the victim in the history
  struct pending_victim {
    bool valid = false;
    long set = 0;
    rlm::decision d{};
    rlm::features victim_features{};
    bool by_brrip = false;
  } pending;
  uint64_t brrip_victims = 0;

  // The last block filled; its sharer count feeds the average threshold once the cache has installed it
  struct filled_block {
    bool valid = false;
    long set = 0;
    long way = 0;
    uint64_t tag = 0;
  } last_fill;

  [[nodiscard]] uint64_t block_tag(champsim::address addr) const;
  [[nodiscard]] std::size_t sharer_count_of(const champsim::cache_block& block) const;
  [[nodiscard]] rlm::features features_of(long set, const champsim::cache_block& block, const rlm::line_meta& meta) const;
  void observe_last_fill();

public:
  explicit rl_mesi(CACHE* cache);
  rl_mesi(CACHE* cache, long sets, long ways);

  long find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                   champsim::address full_addr, access_type type);
  void replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                              access_type type);
  void update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                                access_type type, uint8_t hit);
  void replacement_final_stats();
};

#endif
