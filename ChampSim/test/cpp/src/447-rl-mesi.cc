#include <catch.hpp>

#include "../replacement/rl_mesi/rl_core.h"

namespace
{
constexpr rlm::features S_below{rlm::state_feature::S, false};
constexpr rlm::features E_below{rlm::state_feature::E, false};
constexpr rlm::features M_below{rlm::state_feature::M, false};
constexpr rlm::features S_above{rlm::state_feature::S, true};

rlm::config cfg_with(rlm::mode m, long window = 4, double alpha = 0.5)
{
  rlm::config cfg;
  cfg.m = m;
  cfg.window = window;
  cfg.alpha = alpha;
  return cfg;
}

// Fill every way with consecutive tags starting at first_tag, so way 0 is the least recently used
void fill_all(rlm::policy& p, rlm::set_state& s, uint64_t first_tag)
{
  for (long way = 0; way < static_cast<long>(std::size(s.lines)); ++way)
    p.on_fill(s, way, first_tag + static_cast<uint64_t>(way), std::nullopt);
}

auto table(std::vector<rlm::features> per_way)
{
  return [per_way](long way) { return per_way.at(static_cast<std::size_t>(way)); };
}
} // namespace

TEST_CASE("rl_mesi weights move toward each reward and stay in [-1, 1]")
{
  rlm::weight_table w;
  w.reward(rlm::mode::both, E_below, +1.0, 0.5);
  REQUIRE(w.state_weight(rlm::state_feature::E) == Catch::Approx(0.5));
  REQUIRE(w.share_weight(false) == Catch::Approx(0.5));

  w.reward(rlm::mode::both, E_below, -1.0, 0.5);
  REQUIRE(w.state_weight(rlm::state_feature::E) == Catch::Approx(-0.25));
  REQUIRE(w.score(rlm::mode::both, E_below) == Catch::Approx(-0.5));
  REQUIRE(w.score(rlm::mode::state, E_below) == Catch::Approx(-0.25));
  REQUIRE(w.score(rlm::mode::bitset, E_below) == Catch::Approx(-0.25));

  // Only the components the mode uses are trained
  w.reward(rlm::mode::state, M_below, +1.0, 0.5);
  REQUIRE(w.state_weight(rlm::state_feature::M) == Catch::Approx(0.5));
  REQUIRE(w.share_weight(false) == Catch::Approx(-0.25));
}

TEST_CASE("rl_mesi maps a previously invalidated block to the Iprev state")
{
  REQUIRE(rlm::to_state_feature(rlm::mesi::S, false, true) == rlm::state_feature::S);
  REQUIRE(rlm::to_state_feature(rlm::mesi::M, true, true) == rlm::state_feature::Iprev);
  REQUIRE(rlm::to_state_feature(rlm::mesi::M, true, false) == rlm::state_feature::M);
  REQUIRE(rlm::to_state_feature(rlm::mesi::I, false, true) == rlm::state_feature::none);
}

TEST_CASE("rl_mesi with mode off is exactly LRU")
{
  rlm::policy p{cfg_with(rlm::mode::off)};
  rlm::set_state s{4};

  // Ties go to the lowest way, as replacement/lru
  REQUIRE(p.choose_victim(s, table({S_below, S_below, S_below, S_below})).victim == 0);

  fill_all(p, s, 10);
  p.on_access(s, 0, 10, true, false);
  auto d = p.choose_victim(s, table({M_below, S_below, S_below, S_below}));
  REQUIRE(d.victim == 1);
  REQUIRE_FALSE(d.learned);

  p.commit(s, d);
  REQUIRE(p.stats.decisions == 0);
}

TEST_CASE("rl_mesi evicts the highest-scoring way among the N oldest, ties to the older way")
{
  rlm::weight_table w;
  w.reward(rlm::mode::both, M_below, +1.0, 0.5); // wM = 0.5, wBelow = 0.5
  rlm::policy p{cfg_with(rlm::mode::both, 3), w};
  rlm::set_state s{4};
  fill_all(p, s, 10);

  // Scores: way0 S 0.5, way1 M 1.0, way2 M 1.0, way3 M 1.0 but outside the window of 3
  auto d = p.choose_victim(s, table({S_below, M_below, M_below, M_below}));
  REQUIRE(d.learned);
  REQUIRE(d.victim == 1);
  REQUIRE(std::size(d.passed_over) == 1);
  REQUIRE(d.passed_over.front().first == 0);

  // A window of 1 can only take the oldest way
  rlm::policy narrow{cfg_with(rlm::mode::both, 1), w};
  REQUIRE(narrow.choose_victim(s, table({S_below, M_below, M_below, M_below})).victim == 0);
}

TEST_CASE("rl_mesi choosing a victim changes nothing until the fill commits it")
{
  rlm::weight_table w;
  w.reward(rlm::mode::both, M_below, +1.0, 0.5);
  rlm::policy p{cfg_with(rlm::mode::both), w};
  rlm::set_state s{4};
  fill_all(p, s, 10);

  auto d = p.choose_victim(s, table({S_below, M_below, S_below, S_below}));
  REQUIRE(p.stats.decisions == 0);
  REQUIRE_FALSE(s.lines.at(0).is_protected);

  p.commit(s, d);
  REQUIRE(p.stats.decisions == 1);
  REQUIRE(p.stats.chose_other == 1);
  REQUIRE(s.lines.at(0).is_protected);
}

TEST_CASE("rl_mesi learns from a protected block that is used again")
{
  rlm::weight_table w;
  w.reward(rlm::mode::both, M_below, +1.0, 0.5);
  rlm::policy p{cfg_with(rlm::mode::both), w};
  rlm::set_state s{4};
  fill_all(p, s, 10);

  // Way 0 (S, above threshold) is passed over in favour of way 1
  auto d = p.choose_victim(s, table({S_above, M_below, S_below, S_below}));
  p.commit(s, d);
  p.on_fill(s, d.victim, 20, rlm::history_entry{11, M_below});

  p.on_access(s, 0, 10, true, false);
  REQUIRE(p.stats.protect_good == 1);
  REQUIRE(p.weights().state_weight(rlm::state_feature::S) == Catch::Approx(-0.5));
  REQUIRE(p.weights().share_weight(true) == Catch::Approx(-0.5));
  REQUIRE_FALSE(s.lines.at(0).is_protected);
}

TEST_CASE("rl_mesi learns from a protected block that is evicted without being used")
{
  rlm::weight_table w;
  w.reward(rlm::mode::state, M_below, +1.0, 0.5);
  rlm::policy p{cfg_with(rlm::mode::state, 2), w};
  rlm::set_state s{2};
  fill_all(p, s, 10);

  // Way 0 (E) is protected in favour of way 1 (M)
  auto d1 = p.choose_victim(s, table({E_below, M_below}));
  REQUIRE(d1.victim == 1);
  p.commit(s, d1);
  p.on_fill(s, 1, 20, rlm::history_entry{11, M_below});

  // The new block in way 1 scores the same as way 0, so the older way 0 is evicted while still protected: the protection was wasted
  auto d2 = p.choose_victim(s, table({E_below, E_below}));
  REQUIRE(d2.victim == 0);
  p.commit(s, d2);
  REQUIRE(p.stats.protect_wasted == 1);
  REQUIRE(p.weights().state_weight(rlm::state_feature::E) == Catch::Approx(0.5));
}

TEST_CASE("rl_mesi rewards evictions through the history: -1 when the block returns, +1 when it ages out")
{
  rlm::policy p{cfg_with(rlm::mode::state, 4, 0.5)};
  rlm::set_state s{2}; // history capacity 4
  fill_all(p, s, 10);

  auto d = p.choose_victim(s, table({E_below, E_below}));
  p.commit(s, d);
  p.on_fill(s, d.victim, 20, rlm::history_entry{10, E_below});

  p.on_access(s, 0, 10, false, false);
  REQUIRE(p.stats.evict_bad == 1);
  REQUIRE(p.weights().state_weight(rlm::state_feature::E) == Catch::Approx(-0.5));

  // Five more evictions of blocks that never return overflow the 4-entry history
  for (uint64_t tag = 30; tag < 35; ++tag) {
    auto di = p.choose_victim(s, table({M_below, M_below}));
    p.commit(s, di);
    p.on_fill(s, di.victim, tag + 100, rlm::history_entry{tag, M_below});
  }
  REQUIRE(p.stats.evict_good == 1);
  REQUIRE(p.weights().state_weight(rlm::state_feature::M) == Catch::Approx(0.5));
}

TEST_CASE("rl_mesi detects an invalidated block when its way is reused, and marks it Iprev when it returns")
{
  rlm::policy p{cfg_with(rlm::mode::both)};
  rlm::set_state s{2};
  p.on_fill(s, 0, 100, std::nullopt);

  // Way 0 is filled again without an eviction: block 100 must have been invalidated
  p.on_fill(s, 0, 200, std::nullopt);
  REQUIRE(p.stats.invalidations_seen == 1);
  REQUIRE(std::size(s.invalidated) == 1);

  p.on_fill(s, 1, 100, std::nullopt);
  REQUIRE(p.stats.iprev_fills == 1);
  REQUIRE(s.lines.at(1).previously_invalidated);
  REQUIRE(s.invalidated.empty());
  REQUIRE_FALSE(s.lines.at(0).previously_invalidated);
}

TEST_CASE("rl_mesi detects an invalidated block that returns to a different way")
{
  rlm::policy p{cfg_with(rlm::mode::both)};
  rlm::set_state s{2};
  p.on_fill(s, 0, 100, std::nullopt);
  p.on_fill(s, 1, 100, std::nullopt);

  REQUIRE(p.stats.iprev_fills == 1);
  REQUIRE(s.lines.at(1).previously_invalidated);
  REQUIRE_FALSE(s.lines.at(0).resident);
}

TEST_CASE("rl_mesi does not blame the policy for a miss caused by an invalidation")
{
  rlm::policy p{cfg_with(rlm::mode::both)};
  rlm::set_state s{2};
  p.on_fill(s, 0, 100, std::nullopt);
  p.on_fill(s, 0, 200, std::nullopt); // block 100 was invalidated, not evicted

  p.on_access(s, 0, 100, false, false);
  REQUIRE(p.stats.evict_bad == 0);
}

TEST_CASE("rl_mesi ignores writebacks for recency and rewards")
{
  rlm::policy p{cfg_with(rlm::mode::both)};
  rlm::set_state s{2};
  fill_all(p, s, 10);

  p.on_access(s, 0, 10, true, true);
  REQUIRE(s.lru_order().front() == 0);

  p.on_access(s, 0, 10, true, false);
  REQUIRE(s.lru_order().front() == 1);
}

TEST_CASE("rl_mesi parses its mode")
{
  REQUIRE(rlm::parse_mode("off") == rlm::mode::off);
  REQUIRE(rlm::parse_mode("both") == rlm::mode::both);
  REQUIRE_FALSE(rlm::parse_mode("LRU").has_value());
}

TEST_CASE("rl_mesi parses its sharer source")
{
  REQUIRE(rlm::parse_sharer_source("delivered") == rlm::sharer_source::delivered);
  REQUIRE(rlm::parse_sharer_source("oracle") == rlm::sharer_source::oracle);
  REQUIRE_FALSE(rlm::parse_sharer_source("exact").has_value());
  REQUIRE(rlm::name(rlm::sharer_source::delivered) == "delivered");
  REQUIRE(rlm::name(rlm::sharer_source::oracle) == "oracle");
}

TEST_CASE("rl_mesi threshold is more than half the cores, from a count or from a mask")
{
  REQUIRE_FALSE(rlm::above_threshold(std::size_t{8}, 16));
  REQUIRE(rlm::above_threshold(std::size_t{9}, 16));
  REQUIRE_FALSE(rlm::above_threshold(std::size_t{1}, 2));
  REQUIRE(rlm::above_threshold(std::size_t{2}, 2));
  REQUIRE(rlm::above_threshold(std::size_t{1}, 1));
  REQUIRE_FALSE(rlm::above_threshold(std::size_t{0}, 1));

  REQUIRE(rlm::count_sharers(0b1011) == 3);
  REQUIRE_FALSE(rlm::above_threshold_mask(0xff, 16));
  REQUIRE(rlm::above_threshold_mask(0x1ff, 16));
  REQUIRE(rlm::above_threshold_mask(0x3, 2));
}
