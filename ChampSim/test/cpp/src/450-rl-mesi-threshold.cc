#include <catch.hpp>

#include <set>

#include "../replacement/rl_mesi/threshold.h"

namespace
{
rlm::threshold_config cfg_with(rlm::threshold_mode m, uint64_t epoch = 8192)
{
  rlm::threshold_config cfg;
  cfg.m = m;
  cfg.epoch_accesses = epoch;
  return cfg;
}

// Feeds accesses round-robin over every leader set; candidate i misses on miss_every[i] (1 = always, 0 = never)
void feed_leaders(rlm::sharing_threshold& uut, std::size_t num_sets, const std::vector<int>& miss_every, uint64_t rounds)
{
  std::vector<uint64_t> tick(miss_every.size(), 0);
  for (uint64_t r = 0; r < rounds; ++r)
    for (std::size_t s = 0; s < num_sets; ++s)
      if (auto leader = uut.leader_of(s); leader.has_value()) {
        const int every = miss_every.at(*leader);
        const bool miss = every != 0 && (tick.at(*leader)++ % static_cast<uint64_t>(every)) == 0;
        uut.on_access(s, !miss);
      }
}
} // namespace

TEST_CASE("Fixed mode reproduces the paper's count > num_cpus / 2")
{
  for (std::size_t cpus : {1u, 2u, 4u, 16u}) {
    rlm::sharing_threshold uut{64, cpus, cfg_with(rlm::threshold_mode::fixed)};
    REQUIRE(uut.threshold_for(0) == cpus / 2);
    for (std::size_t set : {std::size_t{0}, std::size_t{63}})
      for (uint32_t count = 0; count <= cpus; ++count)
        REQUIRE(uut.heavy(set, count) == (count > cpus / 2));
    uut.observe(static_cast<uint32_t>(cpus));
    uut.on_access(0, false);
    REQUIRE(uut.threshold_for(0) == cpus / 2);
  }
}

TEST_CASE("Mode names round-trip")
{
  for (auto m : {rlm::threshold_mode::fixed, rlm::threshold_mode::average, rlm::threshold_mode::duel})
    REQUIRE(rlm::parse_threshold_mode(rlm::name(m)) == m);
  REQUIRE_FALSE(rlm::parse_threshold_mode("dynamic").has_value());
}

TEST_CASE("Invalid geometry is rejected")
{
  REQUIRE_THROWS_AS((rlm::sharing_threshold{0, 4}), std::invalid_argument);
  REQUIRE_THROWS_AS((rlm::sharing_threshold{64, 0}), std::invalid_argument);
}

TEST_CASE("Average mode starts at the paper's value and tracks the stream of counts")
{
  rlm::sharing_threshold uut{64, 16, cfg_with(rlm::threshold_mode::average)};
  REQUIRE(uut.threshold_for(5) == 8);

  for (int i = 0; i < 400; ++i)
    uut.observe(2);
  REQUIRE(uut.threshold_for(5) == 2);
  REQUIRE(uut.average() == Catch::Approx(2.0));
  REQUIRE_FALSE(uut.heavy(5, 2));
  REQUIRE(uut.heavy(5, 3));

  for (int i = 0; i < 400; ++i)
    uut.observe(10);
  REQUIRE(uut.threshold_for(0) == 10);

  // A mix of 1s and 3s averages to 2
  for (int i = 0; i < 2000; ++i)
    uut.observe(i % 2 == 0 ? 1 : 3);
  REQUIRE(uut.average() == Catch::Approx(2.0).margin(0.2));
  REQUIRE(uut.heavy(0, 3));
  REQUIRE_FALSE(uut.heavy(0, 1));
}

TEST_CASE("Average mode converges exactly onto a constant stream from below and above")
{
  rlm::sharing_threshold up{64, 16, cfg_with(rlm::threshold_mode::average)};
  for (int i = 0; i < 2000; ++i)
    up.observe(12);
  REQUIRE(up.average() == 12.0);
  REQUIRE_FALSE(up.heavy(0, 12));

  // All private blocks: nothing is heavy (the feature has nothing to tell apart)
  rlm::sharing_threshold down{64, 16, cfg_with(rlm::threshold_mode::average)};
  for (int i = 0; i < 2000; ++i)
    down.observe(1);
  REQUIRE(down.average() == 1.0);
  REQUIRE_FALSE(down.heavy(0, 1));
  REQUIRE(down.heavy(0, 2));
}

TEST_CASE("Average mode clamps counts above num_cpus")
{
  rlm::sharing_threshold uut{64, 4, cfg_with(rlm::threshold_mode::average)};
  for (int i = 0; i < 1000; ++i)
    uut.observe(1000);
  REQUIRE(uut.threshold_for(0) == 4);
}

TEST_CASE("Default duel candidates include the paper's value and stay within K")
{
  using v = std::vector<uint32_t>;
  REQUIRE(rlm::default_threshold_candidates(1, 4) == v{0});
  REQUIRE(rlm::default_threshold_candidates(2, 4) == v{0, 1});
  REQUIRE(rlm::default_threshold_candidates(4, 4) == v{0, 1, 2, 3});
  REQUIRE(rlm::default_threshold_candidates(8, 4) == v{1, 2, 4, 6});
  REQUIRE(rlm::default_threshold_candidates(16, 4) == v{1, 2, 4, 8});
  REQUIRE(rlm::default_threshold_candidates(32, 4) == v{1, 2, 8, 16});
  REQUIRE(rlm::default_threshold_candidates(5, 4) == v{0, 1, 2, 3});
  for (std::size_t cpus = 1; cpus <= 64; ++cpus)
    for (std::size_t k = 1; k <= 6; ++k) {
      auto c = rlm::default_threshold_candidates(cpus, k);
      REQUIRE(c.size() == std::min(cpus, k));
      REQUIRE(std::is_sorted(c.begin(), c.end()));
      REQUIRE(std::adjacent_find(c.begin(), c.end()) == c.end());
      REQUIRE(std::find(c.begin(), c.end(), static_cast<uint32_t>(cpus / 2)) != c.end());
      REQUIRE(c.back() < cpus);
    }
}

TEST_CASE("Duel leaders are disjoint, equally many per candidate, spread, and at most half the sets")
{
  constexpr std::size_t sets = 1024;
  rlm::sharing_threshold uut{sets, 16, cfg_with(rlm::threshold_mode::duel)};
  const auto& cands = uut.candidates();
  REQUIRE(cands.size() == 4);
  REQUIRE(uut.leaders_per_candidate() == 16);

  std::vector<std::vector<std::size_t>> leaders(cands.size());
  for (std::size_t s = 0; s < sets; ++s)
    if (auto l = uut.leader_of(s); l.has_value()) {
      leaders.at(*l).push_back(s);
      REQUIRE(uut.threshold_for(s) == cands.at(*l).threshold);
    } else {
      REQUIRE(uut.threshold_for(s) == 8); // followers start at the paper's value
    }

  std::size_t total = 0;
  for (std::size_t i = 0; i < cands.size(); ++i) {
    REQUIRE(leaders.at(i).size() == 16);
    REQUIRE(cands.at(i).leader_sets == 16);
    // One leader per 64-set constituency
    for (std::size_t g = 0; g < 16; ++g)
      REQUIRE(leaders.at(i).at(g) / 64 == g);
    total += leaders.at(i).size();
  }
  REQUIRE(total <= sets / 2);

  // Low index bits differ across constituencies (not all leaders alias to one bank / stride)
  std::set<std::size_t> low_bits;
  for (auto s : leaders.at(0))
    low_bits.insert(s % 16);
  REQUIRE(low_bits.size() > 1);
}

TEST_CASE("Duel picks the candidate whose leaders miss least, and followers use it")
{
  constexpr std::size_t sets = 1024;
  rlm::sharing_threshold uut{sets, 16, cfg_with(rlm::threshold_mode::duel, 1000)};
  REQUIRE(uut.winner_threshold() == 8);

  // Candidate 1 (t = 2) misses on every 10th access, the others on every other access
  feed_leaders(uut, sets, {2, 10, 2, 2}, 20);
  REQUIRE(uut.epochs() >= 1);
  REQUIRE(uut.winner() == 1);
  REQUIRE(uut.winner_threshold() == 2);
  REQUIRE(uut.candidates().at(1).wins == uut.epochs());

  for (std::size_t s = 0; s < sets; ++s) {
    if (uut.leader_of(s).has_value())
      continue;
    REQUIRE(uut.threshold_for(s) == 2);
    REQUIRE(uut.heavy(s, 3));
    REQUIRE_FALSE(uut.heavy(s, 2));
  }

  // The workload changes: now t = 4 is best; the decayed counts let the duel follow
  feed_leaders(uut, sets, {2, 2, 20, 2}, 200);
  REQUIRE(uut.winner_threshold() == 4);
}

TEST_CASE("Duel ties go to the paper's threshold")
{
  constexpr std::size_t sets = 1024;
  rlm::sharing_threshold uut{sets, 16, cfg_with(rlm::threshold_mode::duel, 640)};
  feed_leaders(uut, sets, {4, 4, 4, 4}, 10);
  REQUIRE(uut.epochs() >= 1);
  REQUIRE(uut.winner_threshold() == 8);

  rlm::sharing_threshold no_miss{sets, 16, cfg_with(rlm::threshold_mode::duel, 640)};
  feed_leaders(no_miss, sets, {0, 0, 0, 0}, 10);
  REQUIRE(no_miss.winner_threshold() == 8);
}

TEST_CASE("Duel decays or resets its counts at each epoch end")
{
  constexpr std::size_t sets = 1024;
  auto decayed_cfg = cfg_with(rlm::threshold_mode::duel, 64);
  rlm::sharing_threshold decayed{sets, 16, decayed_cfg};
  feed_leaders(decayed, sets, {1, 1, 1, 1}, 1); // 64 leader accesses: exactly one epoch
  REQUIRE(decayed.epochs() == 1);
  for (const auto& c : decayed.candidates()) {
    REQUIRE(c.accesses == 8);
    REQUIRE(c.misses == 8);
    REQUIRE(c.total_accesses == 16);
  }

  auto reset_cfg = decayed_cfg;
  reset_cfg.decay = false;
  rlm::sharing_threshold reset{sets, 16, reset_cfg};
  feed_leaders(reset, sets, {1, 1, 1, 1}, 1);
  for (const auto& c : reset.candidates()) {
    REQUIRE(c.accesses == 0);
    REQUIRE(c.total_misses == 16);
  }
}

TEST_CASE("Tiny caches do not duel and keep the paper's value")
{
  for (std::size_t sets : {1u, 4u}) {
    rlm::sharing_threshold uut{sets, 16, cfg_with(rlm::threshold_mode::duel, 4)};
    REQUIRE(uut.leaders_per_candidate() == 0);
    for (int i = 0; i < 100; ++i)
      for (std::size_t s = 0; s < sets; ++s) {
        REQUIRE_FALSE(uut.leader_of(s).has_value());
        uut.on_access(s, i % 2 == 0);
        REQUIRE(uut.threshold_for(s) == 8);
      }
    REQUIRE(uut.epochs() == 0);
  }

  // 8 sets and 4 candidates: one leader each, the other half follow
  rlm::sharing_threshold eight{8, 16, cfg_with(rlm::threshold_mode::duel, 4)};
  REQUIRE(eight.leaders_per_candidate() == 1);
  std::size_t followers = 0;
  for (std::size_t s = 0; s < 8; ++s)
    followers += eight.leader_of(s).has_value() ? 0 : 1;
  REQUIRE(followers == 4);
}

TEST_CASE("One core (no sharing) degenerates to the paper's classification")
{
  for (auto m : {rlm::threshold_mode::fixed, rlm::threshold_mode::average, rlm::threshold_mode::duel}) {
    rlm::sharing_threshold uut{64, 1, cfg_with(m, 4)};
    for (int i = 0; i < 50; ++i) {
      uut.observe(1);
      uut.on_access(static_cast<std::size_t>(i % 64), false);
    }
    REQUIRE_FALSE(uut.heavy(0, 0));
    REQUIRE(uut.leaders_per_candidate() == 0);
    REQUIRE(uut.epochs() == 0);
    if (m != rlm::threshold_mode::average)
      REQUIRE(uut.heavy(0, 1)); // paper: 1 > 1/2 = 0
  }
}

TEST_CASE("Two cores duel between t = 0 and t = 1")
{
  rlm::sharing_threshold uut{64, 2, cfg_with(rlm::threshold_mode::duel, 100)};
  REQUIRE(uut.candidates().size() == 2);
  REQUIRE(uut.winner_threshold() == 1);
  feed_leaders(uut, 64, {10, 2}, 50);
  REQUIRE(uut.winner_threshold() == 0);
  for (std::size_t s = 0; s < 64; ++s)
    if (!uut.leader_of(s).has_value())
      REQUIRE(uut.heavy(s, 1)); // t = 0: any sharer is heavy

}
