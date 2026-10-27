#include <catch.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cache.h"
#include "coherence.h"
#include "defaults.hpp"
#include "mocks.hpp"

/*
 * C3: the directory delivers the sharer count with each grant, and private caches keep it per block (cache_block::delivered_sharers).
 * The stored count is a snapshot taken at grant time: it goes stale when other cores join or leave, and is refreshed to 1 only when
 * this core obtains ownership.
 */

namespace
{
/*
 * N cores, each with a one-way private L1 (and, with WITH_L2, a two-way private L2 below it), feeding a shared coherence point
 * (home) that feeds a DRAM stand-in.
 */
template <std::size_t N, bool WITH_L2 = false, typename DRAM = do_nothing_MRC>
struct hierarchy {
  DRAM dram;
  std::array<champsim::channel, N> private_to_home{}, l1_to_l2{};
  std::array<to_rq_MRP, N> reads{};
  std::array<to_wq_MRP, N> writes{};
  std::unique_ptr<CACHE> home;
  std::array<std::unique_ptr<CACHE>, N> l1{}, l2{};
  std::vector<champsim::operable*> elements;
  uint64_t id = 1;

  hierarchy()
  {
    std::vector<champsim::channel*> upper;
    for (auto& ch : private_to_home)
      upper.push_back(&ch);
    home = std::make_unique<CACHE>(champsim::cache_builder{champsim::defaults::default_llc}
                                       .name("448-home")
                                       .sets(1)
                                       .ways(8)
                                       .upper_levels(std::move(upper))
                                       .lower_level(&dram.queues)
                                       .hit_latency(1)
                                       .fill_latency(1));
    elements.push_back(home.get());
    std::vector<CACHE*> caches{home.get()};
    std::vector<champsim::coherence::entry_point> entries;
    for (std::size_t i = 0; i < N; ++i) {
      if constexpr (WITH_L2) {
        l2[i] = std::make_unique<CACHE>(champsim::cache_builder{champsim::defaults::default_l2c}
                                            .name("448-l2-" + std::to_string(i))
                                            .sets(1)
                                            .ways(2)
                                            .upper_levels({&l1_to_l2[i]})
                                            .lower_level(&private_to_home[i])
                                            .hit_latency(1)
                                            .fill_latency(1));
        elements.push_back(l2[i].get());
        caches.push_back(l2[i].get());
      }
      l1[i] = std::make_unique<CACHE>(champsim::cache_builder{champsim::defaults::default_l1d}
                                          .name("448-l1-" + std::to_string(i))
                                          .sets(1)
                                          .ways(1)
                                          .upper_levels({&reads[i].queues, &writes[i].queues})
                                          .lower_level(WITH_L2 ? &l1_to_l2[i] : &private_to_home[i])
                                          .hit_latency(1)
                                          .fill_latency(1));
      elements.push_back(l1[i].get());
      caches.push_back(l1[i].get());
      entries.push_back({static_cast<uint32_t>(i), &reads[i].queues});
      entries.push_back({static_cast<uint32_t>(i), &writes[i].queues});
    }
    elements.push_back(&dram);
    for (std::size_t i = 0; i < N; ++i) {
      elements.push_back(&reads[i]);
      elements.push_back(&writes[i]);
    }
    champsim::coherence::resolve(caches, entries);
    for (auto* element : elements) {
      element->initialize();
      element->warmup = false;
      element->begin_phase();
    }
  }

  void run(int cycles = 80)
  {
    for (int n = 0; n < cycles; ++n)
      for (auto* element : elements)
        element->_operate();
  }

  static champsim::channel::request_type make_request(champsim::address addr, uint32_t cpu, access_type type, uint64_t instr_id)
  {
    champsim::channel::request_type req;
    req.address = addr;
    req.v_address = addr;
    req.cpu = cpu;
    req.type = type;
    req.instr_id = instr_id;
    return req;
  }

  void issue(uint32_t cpu, champsim::address addr, access_type type = access_type::LOAD)
  {
    auto req = make_request(addr, cpu, type, id++);
    REQUIRE((type == access_type::WRITE ? writes[cpu].issue(req) : reads[cpu].issue(req)));
  }

  void access(uint32_t cpu, champsim::address addr, access_type type = access_type::LOAD)
  {
    issue(cpu, addr, type);
    run();
  }

  [[nodiscard]] std::size_t oracle_count(champsim::address addr) const
  {
    return champsim::coherence::directory::popcount(home->coherence_directory->sharers_of(addr));
  }
};

std::optional<champsim::cache_block> find_block(const CACHE& cache, champsim::address addr)
{
  auto it = std::find_if(std::begin(cache.block), std::end(cache.block),
                         [addr](const auto& blk) { return blk.valid && champsim::block_number{blk.address} == champsim::block_number{addr}; });
  if (it == std::end(cache.block))
    return std::nullopt;
  return *it;
}

uint32_t delivered(const CACHE& cache, champsim::address addr)
{
  auto block = find_block(cache, addr);
  REQUIRE(block.has_value());
  return block->delivered_sharers;
}
} // namespace

TEST_CASE("C3: the first reader is told it is the only sharer", "[mesi-delivered-sharers]")
{
  hierarchy<2> h;
  const champsim::address x{0x1000};
  h.access(0, x);
  CHECK(delivered(*h.l1[0], x) == 1);
  CHECK(h.oracle_count(x) == 1);
}

TEST_CASE("C3: a second reader is told two cores share the block; the first reader's count stays stale", "[mesi-delivered-sharers]")
{
  hierarchy<2> h;
  const champsim::address x{0x1000};
  h.access(0, x);
  h.access(1, x); // served by a hit at the coherence point

  CHECK(delivered(*h.l1[1], x) == 2);
  CHECK(delivered(*h.l1[0], x) == 1); // not told about core 1 joining
  CHECK(h.oracle_count(x) == 2);
  CHECK(find_block(*h.l1[0], x)->state == champsim::coherence_state::SHARED); // the state, unlike the count, is downgraded
}

TEST_CASE("C3: each successive reader is told the sharer count at its own grant", "[mesi-delivered-sharers]")
{
  hierarchy<4> h;
  const champsim::address x{0x1000};
  for (uint32_t cpu = 0; cpu < 4; ++cpu)
    h.access(cpu, x);
  for (uint32_t cpu = 0; cpu < 4; ++cpu)
    CHECK(delivered(*h.l1[cpu], x) == cpu + 1);
  CHECK(h.oracle_count(x) == 4);
}

TEST_CASE("C3: a writer that upgrades a shared copy is told it is the only sharer", "[mesi-delivered-sharers]")
{
  hierarchy<2> h;
  const champsim::address x{0x1000};
  h.access(0, x);
  h.access(1, x);
  REQUIRE(delivered(*h.l1[1], x) == 2);

  h.access(1, x, access_type::WRITE); // store hit on SHARED: upgrade, core 0 invalidated
  CHECK(find_block(*h.l1[1], x)->state == champsim::coherence_state::MODIFIED);
  CHECK(delivered(*h.l1[1], x) == 1);
  CHECK_FALSE(find_block(*h.l1[0], x).has_value());
  CHECK(h.oracle_count(x) == 1);
}

TEST_CASE("C3: a writer that misses is granted MODIFIED with a count of 1 after invalidating the readers", "[mesi-delivered-sharers]")
{
  hierarchy<3> h;
  const champsim::address x{0x1000};
  h.access(0, x);
  h.access(1, x);
  h.access(2, x, access_type::WRITE);

  CHECK(find_block(*h.l1[2], x)->state == champsim::coherence_state::MODIFIED);
  CHECK(delivered(*h.l1[2], x) == 1);
  CHECK_FALSE(find_block(*h.l1[0], x).has_value());
  CHECK_FALSE(find_block(*h.l1[1], x).has_value());
  CHECK(h.oracle_count(x) == 1);
}

TEST_CASE("C3: a count stays stale when another sharer leaves", "[mesi-delivered-sharers]")
{
  hierarchy<2> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x);
  h.access(1, x);
  h.access(0, y); // core 0's one-way L1 evicts x

  CHECK_FALSE(find_block(*h.l1[0], x).has_value());
  CHECK(h.oracle_count(x) == 1);
  CHECK(delivered(*h.l1[1], x) == 2); // core 1 was never told that core 0 left
}

TEST_CASE("C3: the count travels from the coherence point through L2 to L1", "[mesi-delivered-sharers]")
{
  hierarchy<2, true> h;
  const champsim::address x{0x1000}, y{0x2000};

  h.access(0, x); // fill path at every level
  CHECK(delivered(*h.l2[0], x) == 1);
  CHECK(delivered(*h.l1[0], x) == 1);

  h.access(1, x); // hit at the coherence point, fills at L2 and L1
  CHECK(delivered(*h.l2[1], x) == 2);
  CHECK(delivered(*h.l1[1], x) == 2);

  // Core 0's L1 loses x but its L2 keeps it; the L2 hit forwards the count it stored (1), not the current one (2)
  h.access(0, y);
  REQUIRE_FALSE(find_block(*h.l1[0], x).has_value());
  REQUIRE(find_block(*h.l2[0], x).has_value());
  h.access(0, x);
  CHECK(delivered(*h.l1[0], x) == 1);
  CHECK(h.oracle_count(x) == 2);
}

TEST_CASE("C3: an upgrade refreshes the count in every private level of the writer", "[mesi-delivered-sharers]")
{
  hierarchy<2, true> h;
  const champsim::address x{0x1000};
  h.access(0, x);
  h.access(1, x);
  REQUIRE(delivered(*h.l1[1], x) == 2);
  REQUIRE(delivered(*h.l2[1], x) == 2);

  h.access(1, x, access_type::WRITE);
  CHECK(delivered(*h.l1[1], x) == 1);
  CHECK(delivered(*h.l2[1], x) == 1);
}

TEST_CASE("C3: reads merged at the coherence point are granted in turn, each with its own count", "[mesi-delivered-sharers]")
{
  hierarchy<2, false, release_MRC> h;
  const champsim::address x{0x1000};
  h.issue(0, x);
  h.issue(1, x);
  h.run(20);
  h.dram.release_all();
  h.run(80);

  std::array<uint32_t, 2> counts{delivered(*h.l1[0], x), delivered(*h.l1[1], x)};
  std::sort(std::begin(counts), std::end(counts));
  CHECK(counts == std::array<uint32_t, 2>{1, 2});
  CHECK(h.oracle_count(x) == 2);
}

TEST_CASE("C3: the coherence point keeps the exact mask and no delivered count", "[mesi-delivered-sharers]")
{
  hierarchy<2> h;
  const champsim::address x{0x1000};
  h.access(0, x);
  h.access(1, x);
  auto home_block = find_block(*h.home, x);
  REQUIRE(home_block.has_value());
  CHECK(home_block->sharers == 0b11);
  CHECK(home_block->delivered_sharers == 0);
}
