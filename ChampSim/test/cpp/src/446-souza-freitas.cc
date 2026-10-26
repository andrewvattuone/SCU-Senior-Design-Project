#include <catch.hpp>

#include "../replacement/souza_freitas/sf_core.h"

/*
 * M. A. Souza and H. C. Freitas, "Reinforcement Learning-Based Cache Replacement Policies for Multicore Processors,"
 * IEEE Access, vol. 12, 2024.
 */

namespace
{
sf::eviction_history history_of(std::initializer_list<sf::history_entry> entries, std::size_t capacity = 16)
{
  sf::eviction_history h{capacity};
  for (const auto& e : entries)
    h.insert(e.tag, e.state, e.above);
  return h;
}

// Run select_victim with fixed first/second choices and a per-way candidate table
long decide(sf::mode m, sf::rule r, const sf::eviction_history& h, std::vector<sf::candidate_info> ways, sf::statistics& stats)
{
  return sf::select_victim(
      m, r, h, static_cast<long>(std::size(ways)), [] { return 0L; }, [](long) { return 1L; }, [&](long w) { return ways.at(static_cast<std::size_t>(w)); },
      stats);
}
} // namespace

TEST_CASE("Souza-Freitas weights are ratios of history entries and each group sums to 1")
{
  auto h = history_of({{1, sf::mesi::M, true}, {2, sf::mesi::M, false}, {3, sf::mesi::E, false}, {4, sf::mesi::S, false}});
  auto w = sf::compute_weights(h);
  REQUIRE(w.m == Catch::Approx(0.5));
  REQUIRE(w.e == Catch::Approx(0.25));
  REQUIRE(w.s == Catch::Approx(0.25));
  REQUIRE(w.m + w.e + w.s == Catch::Approx(1.0));
  REQUIRE(w.at == Catch::Approx(0.25));
  REQUIRE(w.bt == Catch::Approx(0.75));
  REQUIRE(w.at + w.bt == Catch::Approx(1.0));
  REQUIRE_FALSE(w.state_empty);
  REQUIRE_FALSE(w.bitset_empty);
}

TEST_CASE("Souza-Freitas threshold is more than half the cores")
{
  REQUIRE_FALSE(sf::above_threshold(0xff, 16));  // 8 of 16
  REQUIRE(sf::above_threshold(0x1ff, 16));       // 9 of 16
  REQUIRE_FALSE(sf::above_threshold(0x1, 2));    // 1 of 2
  REQUIRE(sf::above_threshold(0x3, 2));          // 2 of 2
}

TEST_CASE("Souza-Freitas empty history evicts Exclusive / below-threshold blocks regardless of rule")
{
  sf::eviction_history empty{16};
  auto w = sf::compute_weights(empty);
  REQUIRE(w.state_empty);
  REQUIRE(w.bitset_empty);

  for (auto r : {sf::rule::text, sf::rule::example}) {
    REQUIRE(sf::should_evict(sf::mode::state, r, w, sf::mesi::E, false));
    REQUIRE_FALSE(sf::should_evict(sf::mode::state, r, w, sf::mesi::M, false));
    REQUIRE_FALSE(sf::should_evict(sf::mode::state, r, w, sf::mesi::S, false));
    REQUIRE(sf::should_evict(sf::mode::bitset, r, w, sf::mesi::M, false));
    REQUIRE_FALSE(sf::should_evict(sf::mode::bitset, r, w, sf::mesi::E, true));
  }

  sf::statistics stats;
  REQUIRE(decide(sf::mode::state, sf::rule::text, empty, {{sf::mesi::M, false}, {sf::mesi::M, false}}, stats) == 1);
  REQUIRE(stats.empty_history_decisions == 1);
  REQUIRE(stats.second_chances == 1);
}

TEST_CASE("Souza-Freitas RULE=text evicts the highest-weight state, RULE=example keeps it")
{
  // wM is the highest, as in the paper's §II-C example
  auto h = history_of({{1, sf::mesi::M, false}, {2, sf::mesi::M, false}, {3, sf::mesi::E, false}});
  auto w = sf::compute_weights(h);

  REQUIRE(sf::should_evict(sf::mode::state, sf::rule::text, w, sf::mesi::M, false));
  REQUIRE_FALSE(sf::should_evict(sf::mode::state, sf::rule::text, w, sf::mesi::E, false));

  REQUIRE_FALSE(sf::should_evict(sf::mode::state, sf::rule::example, w, sf::mesi::M, false));
  REQUIRE(sf::should_evict(sf::mode::state, sf::rule::example, w, sf::mesi::E, false));

  // The example: LRU picks an M block, wM is highest, so the second LRU block is chosen
  sf::statistics stats;
  REQUIRE(decide(sf::mode::state, sf::rule::example, h, {{sf::mesi::M, false}, {sf::mesi::E, false}}, stats) == 1);
  REQUIRE(decide(sf::mode::state, sf::rule::text, h, {{sf::mesi::M, false}, {sf::mesi::E, false}}, stats) == 0);
}

TEST_CASE("Souza-Freitas ties count as highest")
{
  auto h = history_of({{1, sf::mesi::M, true}, {2, sf::mesi::E, false}});
  auto w = sf::compute_weights(h);
  REQUIRE(sf::state_says_evict(w, sf::rule::text, sf::mesi::M));
  REQUIRE(sf::state_says_evict(w, sf::rule::text, sf::mesi::E));
  REQUIRE_FALSE(sf::state_says_evict(w, sf::rule::text, sf::mesi::S));
  REQUIRE(sf::bitset_says_evict(w, sf::rule::text, true));
  REQUIRE(sf::bitset_says_evict(w, sf::rule::text, false));
  REQUIRE_FALSE(sf::bitset_says_evict(w, sf::rule::example, true));
}

TEST_CASE("Souza-Freitas RL-Both evicts only when both checks agree")
{
  // State: E highest. Bitset: above-threshold highest.
  auto h = history_of({{1, sf::mesi::E, true}, {2, sf::mesi::E, true}, {3, sf::mesi::M, false}});
  auto w = sf::compute_weights(h);

  REQUIRE(sf::should_evict(sf::mode::both, sf::rule::text, w, sf::mesi::E, true));
  REQUIRE_FALSE(sf::should_evict(sf::mode::both, sf::rule::text, w, sf::mesi::E, false)); // bitset says keep
  REQUIRE_FALSE(sf::should_evict(sf::mode::both, sf::rule::text, w, sf::mesi::M, true));  // state says keep
  REQUIRE_FALSE(sf::should_evict(sf::mode::both, sf::rule::text, w, sf::mesi::M, false));

  REQUIRE(sf::should_evict(sf::mode::state, sf::rule::text, w, sf::mesi::E, false));
  REQUIRE(sf::should_evict(sf::mode::bitset, sf::rule::text, w, sf::mesi::M, true));
}

TEST_CASE("Souza-Freitas INVALID state means no information and accepts candidate #1")
{
  auto h = history_of({{1, sf::mesi::M, false}});
  sf::statistics stats;
  REQUIRE(decide(sf::mode::state, sf::rule::text, h, {{sf::mesi::I, false}, {sf::mesi::E, false}}, stats) == 0);
  REQUIRE(stats.first_accepted == 1);
}

TEST_CASE("Souza-Freitas history holds 2x associativity and drops the least recently updated entry")
{
  constexpr long ways = 4;
  sf::eviction_history h{2 * ways};
  for (uint64_t tag = 0; tag < 2 * ways; ++tag)
    REQUIRE_FALSE(h.insert(tag, sf::mesi::E, false));
  REQUIRE(h.size() == 2 * ways);

  REQUIRE(h.insert(100, sf::mesi::M, true));
  REQUIRE(h.size() == 2 * ways);
  REQUIRE_FALSE(h.contains(0));
  REQUIRE(h.contains(1));
  REQUIRE(h.contains(100));

  // Re-recording a tag makes it the most recently updated
  REQUIRE_FALSE(h.insert(1, sf::mesi::S, false));
  REQUIRE(h.insert(101, sf::mesi::S, false));
  REQUIRE(h.contains(1));
  REQUIRE_FALSE(h.contains(2));
}

TEST_CASE("Souza-Freitas refill removes the history entry")
{
  auto h = history_of({{7, sf::mesi::M, false}, {8, sf::mesi::E, false}});
  REQUIRE(h.remove(7));
  REQUIRE_FALSE(h.contains(7));
  REQUIRE_FALSE(h.remove(7));
  REQUIRE(sf::compute_weights(h).e == Catch::Approx(1.0));
}

TEST_CASE("Souza-Freitas history hit gives reward -1 and removes the entry; a miss gives +1")
{
  auto h = history_of({{7, sf::mesi::M, false}});
  sf::statistics stats;
  REQUIRE(sf::observe_access(h, 7, stats) == -1);
  REQUIRE_FALSE(h.contains(7));
  REQUIRE(stats.history_hits == 1);
  REQUIRE(sf::observe_access(h, 7, stats) == +1);
  REQUIRE(stats.positive_rewards == 1);
}

TEST_CASE("Souza-Freitas LRU helper picks the least and second-least recently used ways")
{
  sf::lru_set uut{4};
  uut.touch(0, 10);
  uut.touch(1, 3);
  uut.touch(2, 7);
  uut.touch(3, 5);
  REQUIRE(uut.victim() == 1);
  REQUIRE(uut.victim(1) == 3);

  // Untouched ways tie at 0; the lowest way wins, as in replacement/lru
  sf::lru_set fresh{4};
  REQUIRE(fresh.victim() == 0);
  REQUIRE(fresh.victim(0) == 1);
}

TEST_CASE("Souza-Freitas SRRIP helper matches the published work for its first choice")
{
  sf::srrip_set uut{4};
  uut.update(0, false); // a1
  uut.update(1, false); // a2
  uut.update(1, true);  // a2
  uut.update(0, true);  // a1
  uut.update(2, false); // b1
  uut.update(3, false); // b2
  REQUIRE(uut.victim() == 2);
}

TEST_CASE("Souza-Freitas SRRIP second chance protects the rejected way and searches the others")
{
  sf::srrip_set uut{4};
  uut.update(0, true);  // 0
  uut.update(1, false); // 2
  uut.update(2, false); // 2
  uut.update(3, true);  // 0

  auto first = uut.victim(); // ages everything by 1
  REQUIRE(first == 1);
  REQUIRE(uut.rrpv == std::vector<int>{1, 3, 3, 1});

  auto second = uut.second_victim(first);
  REQUIRE(second == 2);
  REQUIRE(uut.get(first) == sf::srrip_set::maxRRPV - 1);

  // When no other way is at max, the others are aged but the protected way is not
  sf::srrip_set ages{3};
  ages.update(0, false); // 2
  ages.update(1, true);  // 0
  ages.update(2, true);  // 0
  REQUIRE(ages.victim() == 0);
  REQUIRE(ages.second_victim(0) == 1);
  REQUIRE(ages.rrpv == std::vector<int>{2, 3, 3});
}

TEST_CASE("Souza-Freitas mode off returns the base policy's choice and records no decisions")
{
  auto h = history_of({{1, sf::mesi::E, false}});
  sf::statistics stats;
  // Candidate #1 would be rejected by every RL mode here
  REQUIRE(decide(sf::mode::off, sf::rule::text, h, {{sf::mesi::M, true}, {sf::mesi::E, false}}, stats) == 0);
  REQUIRE(stats.decisions == 0);

  // Same base policy driven through select_victim in off mode matches the plain helper
  sf::lru_set lru{4};
  lru.touch(0, 4);
  lru.touch(1, 2);
  lru.touch(2, 9);
  lru.touch(3, 6);
  auto chosen = sf::select_victim(
      sf::mode::off, sf::rule::text, h, 4, [&] { return lru.victim(); }, [&](long r) { return lru.victim(r); },
      [](long) { return sf::candidate_info{sf::mesi::M, true}; }, stats);
  REQUIRE(chosen == lru.victim());
}

TEST_CASE("Souza-Freitas parses its environment configuration")
{
  REQUIRE(sf::parse_mode("both") == sf::mode::both);
  REQUIRE(sf::parse_mode("off") == sf::mode::off);
  REQUIRE_FALSE(sf::parse_mode("bogus").has_value());
  REQUIRE(sf::parse_base("srrip") == sf::base_policy::srrip);
  REQUIRE(sf::parse_rule("example") == sf::rule::example);
  REQUIRE_FALSE(sf::parse_rule("").has_value());
}
