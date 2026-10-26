#ifndef REPLACEMENT_SOUZA_FREITAS_H
#define REPLACEMENT_SOUZA_FREITAS_H

#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"
#include "sf_core.h"

/*
 * Souza & Freitas (IEEE Access 2024) RL-based replacement: a base policy (LRU or SRRIP) whose first candidate is checked against
 * weights learned from a per-set eviction history of coherence states / sharer counts. See README.md for interpretation choices.
 * Configured at run time with SF_MODE, SF_BASE and SF_RULE.
 */
class souza_freitas : public champsim::modules::replacement
{
  long NUM_SET, NUM_WAY;
  sf::mode mode;
  sf::base_policy base;
  sf::rule rule;

  std::vector<sf::eviction_history> history;
  std::vector<sf::lru_set> lru_sets;
  std::vector<sf::srrip_set> srrip_sets;
  uint64_t cycle = 0;

  // Victim chosen by find_victim, recorded into the history when replacement_cache_fill learns its address
  struct pending_victim {
    bool valid = false;
    long set = 0;
    long way = 0;
    sf::candidate_info info{};
  } pending;

  sf::statistics stats;

  [[nodiscard]] uint64_t block_tag(champsim::address addr) const;
  [[nodiscard]] sf::candidate_info candidate(const champsim::cache_block& block) const;

public:
  explicit souza_freitas(CACHE* cache);
  souza_freitas(CACHE* cache, long sets, long ways);

  long find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                   champsim::address full_addr, access_type type);
  void replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                              access_type type);
  void update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                                access_type type, uint8_t hit);
  void replacement_final_stats();
};

#endif
