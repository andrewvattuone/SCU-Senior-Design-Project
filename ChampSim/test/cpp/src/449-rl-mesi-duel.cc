#include <catch.hpp>

#include "../replacement/rl_mesi/duel.h"

namespace
{
// drrip's leader categories (inc/msl/stat_methods.h): 0 = BRRIP leader, 1 = SRRIP leader (RL takes SRRIP's place here)
long drrip_category(long set, long rate)
{
  long low = set % rate;
  long high = (set / rate) % rate;
  return (rate + low - high) % rate;
}

long count_role(const rlm::set_dueler& d, rlm::set_role r)
{
  long n = 0;
  for (long s = 0; s < d.num_sets(); ++s)
    n += d.role(s) == r ? 1 : 0;
  return n;
}

void miss_n(rlm::set_dueler& d, long set, int n)
{
  for (int i = 0; i < n; ++i)
    d.on_miss(set);
}

long first_set_with(const rlm::set_dueler& d, rlm::set_role r)
{
  for (long s = 0; s < d.num_sets(); ++s)
    if (d.role(s) == r)
      return s;
  return -1;
}
} // namespace

TEST_CASE("Leader sets match drrip's placement for power-of-two caches")
{
  for (auto [sets, rate] : {std::pair{64L, 8L}, {256L, 16L}, {1024L, 32L}, {2048L, 32L}, {4096L, 32L}}) {
    rlm::set_dueler d{sets};
    REQUIRE(d.leaders_per_policy() == sets / rate);
    for (long s = 0; s < sets; ++s) {
      auto cat = drrip_category(s, rate);
      auto expected = cat == 0 ? rlm::set_role::leader_brrip : cat == 1 ? rlm::set_role::leader_rl : rlm::set_role::follower;
      REQUIRE(d.role(s) == expected);
    }
  }
}

TEST_CASE("Leader counts are exact, disjoint and one pair per constituency")
{
  rlm::set_dueler d{2048, 64};
  REQUIRE(count_role(d, rlm::set_role::leader_rl) == 64);
  REQUIRE(count_role(d, rlm::set_role::leader_brrip) == 64);
  REQUIRE(count_role(d, rlm::set_role::follower) == 2048 - 128);
  for (long c = 0; c < 64; ++c) {
    long rl = 0, br = 0;
    for (long s = c * 32; s < (c + 1) * 32; ++s) {
      rl += d.role(s) == rlm::set_role::leader_rl ? 1 : 0;
      br += d.role(s) == rlm::set_role::leader_brrip ? 1 : 0;
    }
    REQUIRE(rl == 1);
    REQUIRE(br == 1);
  }

  rlm::set_dueler odd{100, 7};
  REQUIRE(count_role(odd, rlm::set_role::leader_rl) == 7);
  REQUIRE(count_role(odd, rlm::set_role::leader_brrip) == 7);
}

TEST_CASE("PSEL saturates at both ends")
{
  rlm::set_dueler d{64, 8, 4};
  REQUIRE(d.psel_limit() == 15);
  REQUIRE(d.threshold() == 7);
  auto rl = first_set_with(d, rlm::set_role::leader_rl);
  auto br = first_set_with(d, rlm::set_role::leader_brrip);

  miss_n(d, br, 5);
  REQUIRE(d.psel() == 0);
  miss_n(d, rl, 100);
  REQUIRE(d.psel() == 15);
  miss_n(d, br, 100);
  REQUIRE(d.psel() == 0);
  REQUIRE(d.stats.psel_min == 0);
  REQUIRE(d.stats.psel_max == 15);
}

TEST_CASE("Misses in RL leaders move followers to BRRIP and misses in BRRIP leaders move them back")
{
  rlm::set_dueler d{2048};
  auto rl = first_set_with(d, rlm::set_role::leader_rl);
  auto br = first_set_with(d, rlm::set_role::leader_brrip);
  auto fol = first_set_with(d, rlm::set_role::follower);

  REQUIRE(d.use_rl(fol)); // PSEL starts at 0, as in drrip
  miss_n(d, rl, static_cast<int>(d.threshold()) - 1);
  REQUIRE(d.use_rl(fol));
  d.on_miss(rl);
  REQUIRE_FALSE(d.use_rl(fol));
  REQUIRE(d.stats.preference_flips == 1);

  // Leaders never follow PSEL
  REQUIRE(d.use_rl(rl));
  REQUIRE_FALSE(d.use_rl(br));

  // Follower misses are counted but leave PSEL alone
  auto before = d.psel();
  miss_n(d, fol, 10);
  REQUIRE(d.psel() == before);
  REQUIRE(d.stats.misses_follower_brrip == 10);

  d.on_miss(br);
  REQUIRE(d.use_rl(fol));
  REQUIRE(d.stats.preference_flips == 2);
  d.on_miss(fol);
  REQUIRE(d.stats.misses_follower_rl == 1);
  REQUIRE(d.stats.misses_leader_rl == d.threshold());
  REQUIRE(d.stats.misses_leader_brrip == 1);
  REQUIRE(d.stats.psel_samples == d.threshold() + 12);
}

TEST_CASE("PSEL statistics track the mean")
{
  rlm::set_dueler d{64, 8, 4};
  auto rl = first_set_with(d, rlm::set_role::leader_rl);
  miss_n(d, rl, 4); // samples 1, 2, 3, 4
  REQUIRE(d.stats.psel_mean() == Catch::Approx(2.5));
  REQUIRE(d.stats.psel_min == 1);
  REQUIRE(d.stats.psel_max == 4);
}

TEST_CASE("Tiny caches")
{
  SECTION("1 set: no leaders, always RL, PSEL never moves")
  {
    rlm::set_dueler d{1};
    REQUIRE(d.leaders_per_policy() == 0);
    REQUIRE(d.role(0) == rlm::set_role::follower);
    miss_n(d, 0, 1000);
    REQUIRE(d.psel() == 0);
    REQUIRE(d.use_rl(0));
  }
  SECTION("4 sets by default: one leader per side, two followers")
  {
    rlm::set_dueler d{4};
    REQUIRE(d.leaders_per_policy() == 1);
    REQUIRE(d.role(0) == rlm::set_role::leader_brrip);
    REQUIRE(d.role(1) == rlm::set_role::leader_rl);
    REQUIRE(d.role(2) == rlm::set_role::follower);
    REQUIRE(d.role(3) == rlm::set_role::follower);
    miss_n(d, 1, static_cast<int>(d.threshold()));
    REQUIRE_FALSE(d.use_rl(2));
    REQUIRE_FALSE(d.use_rl(3));
  }
  SECTION("4 sets asking for too many leaders: clamped to 2 per side, all leaders")
  {
    rlm::set_dueler d{4, 8};
    REQUIRE(d.leaders_per_policy() == 2);
    REQUIRE(count_role(d, rlm::set_role::leader_rl) == 2);
    REQUIRE(count_role(d, rlm::set_role::leader_brrip) == 2);
  }
  SECTION("Bad arguments are rejected")
  {
    REQUIRE_THROWS_AS(rlm::set_dueler(0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(rlm::set_dueler(8, -1), std::invalid_argument);
    REQUIRE_THROWS_AS(rlm::set_dueler(8, 1, 0), std::invalid_argument);
  }
}

TEST_CASE("BRRIP inserts at distant RRPV except every 32nd fill")
{
  rlm::brrip_policy p{1, 1};
  int distant = 0, long_rr = 0;
  for (int i = 1; i <= 320; ++i) {
    p.on_fill(0, 0);
    auto r = p.set(0).rrpv(0);
    REQUIRE((r == rlm::BRRIP_MAX_RRPV || r == rlm::BRRIP_MAX_RRPV - 1));
    if (r == rlm::BRRIP_MAX_RRPV - 1) {
      ++long_rr;
      REQUIRE(i % 32 == 0); // as drrip: the counter reaches BRRIP_MAX on the 32nd insertion
    } else {
      ++distant;
    }
  }
  REQUIRE(long_rr == 10);
  REQUIRE(distant == 310);
}

TEST_CASE("BRRIP throttle is shared by all sets of a cache")
{
  rlm::brrip_policy p{4, 1};
  for (int i = 0; i < 31; ++i)
    p.on_fill(i % 3, 0);
  p.on_fill(3, 0);
  REQUIRE(p.set(3).rrpv(0) == rlm::BRRIP_MAX_RRPV - 1);
}

TEST_CASE("BRRIP victim is the first way at max RRPV, aging the set when none is")
{
  rlm::brrip_set s{4};

  SECTION("A way already at max is chosen without aging")
  {
    s.on_fill(2, false);
    s.on_fill(3, false);
    REQUIRE(s.victim() == 2);
    REQUIRE(s.rrpv(0) == 0);
    REQUIRE(s.rrpv(1) == 0);
  }
  SECTION("Aging adds the same amount to every way until one is at max")
  {
    s.on_writeback(1); // 2
    s.on_fill(2, true); // 2
    s.on_hit(2);       // 0
    s.on_writeback(3); // 2
    // rrpv = {0, 2, 0, 2}
    REQUIRE(s.peek_victim() == 1);
    REQUIRE(s.rrpv(1) == 2);
    REQUIRE(s.victim() == 1);
    REQUIRE(s.rrpv(0) == 1);
    REQUIRE(s.rrpv(1) == 3);
    REQUIRE(s.rrpv(2) == 1);
    REQUIRE(s.rrpv(3) == 3);
    // A retried fill asks again: same victim, no further aging
    REQUIRE(s.victim() == 1);
    REQUIRE(s.rrpv(0) == 1);
  }
  SECTION("All ways at 0: aged by maxRRPV, lowest way wins")
  {
    REQUIRE(s.victim() == 0);
    for (long w = 0; w < 4; ++w)
      REQUIRE(s.rrpv(w) == rlm::BRRIP_MAX_RRPV);
  }
}

TEST_CASE("BRRIP hit promotes to 0 and writebacks go to maxRRPV - 1")
{
  rlm::brrip_set s{2};
  s.on_fill(0, false);
  s.on_fill(1, false);
  s.on_hit(1);
  REQUIRE(s.rrpv(1) == 0);
  REQUIRE(s.victim() == 0);
  s.on_writeback(0);
  REQUIRE(s.rrpv(0) == rlm::BRRIP_MAX_RRPV - 1);
}
