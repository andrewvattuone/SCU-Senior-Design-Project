#include <catch.hpp>

#include <algorithm>
#include <array>
#include <optional>

#include "cache.h"
#include "coherence.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
/*
 * A two-core hierarchy: each core has one private cache (cpu 0 -> p0, cpu 1 -> p1) that feeds a shared cache (llc), which
 * feeds a DRAM stand-in. Private caches have one set and one way so that evictions are easy to provoke.
 *
 *   core0 (to_rq/to_wq) --> p0 --\
 *                                 llc --> dram
 *   core1 (to_rq/to_wq) --> p1 --/
 */
template <typename DRAM = do_nothing_MRC>
struct two_core_hierarchy {
  static constexpr uint64_t hit_latency = 1;
  static constexpr uint64_t fill_latency = 1;

  DRAM dram{};
  champsim::channel p0_to_llc{}, p1_to_llc{};
  to_rq_MRP core0_loads{}, core1_loads{};
  to_wq_MRP core0_stores{}, core1_stores{};

  CACHE llc{champsim::cache_builder{champsim::defaults::default_llc}
                .name("440-llc")
                .sets(1)
                .ways(8)
                .upper_levels({{&p0_to_llc, &p1_to_llc}})
                .lower_level(&dram.queues)
                .hit_latency(hit_latency)
                .fill_latency(fill_latency)};

  CACHE p0{champsim::cache_builder{champsim::defaults::default_l1d}
               .name("440-p0")
               .sets(1)
               .ways(1)
               .upper_levels({{&core0_loads.queues, &core0_stores.queues}})
               .lower_level(&p0_to_llc)
               .hit_latency(hit_latency)
               .fill_latency(fill_latency)};

  CACHE p1{champsim::cache_builder{champsim::defaults::default_l1d}
               .name("440-p1")
               .sets(1)
               .ways(1)
               .upper_levels({{&core1_loads.queues, &core1_stores.queues}})
               .lower_level(&p1_to_llc)
               .hit_latency(hit_latency)
               .fill_latency(fill_latency)};

  std::array<champsim::operable*, 8> elements{{&llc, &p0, &p1, &dram, &core0_loads, &core1_loads, &core0_stores, &core1_stores}};

  two_core_hierarchy()
  {
    champsim::coherence::resolve({&llc, &p0, &p1}, {{0, &core0_loads.queues}, {0, &core0_stores.queues}, {1, &core1_loads.queues}, {1, &core1_stores.queues}});

    for (auto* elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }
  }

  void run(int cycles = 40)
  {
    for (int i = 0; i < cycles; ++i) {
      for (auto* elem : elements) {
        elem->_operate();
      }
    }
  }

  static champsim::channel::request_type make_request(champsim::address addr, uint32_t cpu, access_type type, uint64_t id)
  {
    champsim::channel::request_type req;
    req.address = addr;
    req.v_address = addr;
    req.cpu = cpu;
    req.type = type;
    req.instr_id = id;
    return req;
  }

  uint64_t next_id = 1;

  void load(uint32_t cpu, champsim::address addr)
  {
    auto req = make_request(addr, cpu, access_type::LOAD, next_id++);
    REQUIRE((cpu == 0 ? core0_loads : core1_loads).issue(req));
    run();
  }

  void store(uint32_t cpu, champsim::address addr)
  {
    auto req = make_request(addr, cpu, access_type::WRITE, next_id++);
    REQUIRE((cpu == 0 ? core0_stores : core1_stores).issue(req));
    run();
  }
};

std::optional<champsim::cache_block> find_block(const CACHE& cache, champsim::address addr)
{
  auto it = std::find_if(std::begin(cache.block), std::end(cache.block),
                         [addr](const auto& blk) { return blk.valid && champsim::block_number{blk.address} == champsim::block_number{addr}; });
  if (it == std::end(cache.block)) {
    return std::nullopt;
  }
  return *it;
}
} // namespace

SCENARIO("Topology resolution finds the coherence point and its members")
{
  GIVEN("A two-core hierarchy")
  {
    two_core_hierarchy<> h;

    THEN("The shared cache is the coherence point and the private caches are members")
    {
      REQUIRE(h.llc.is_coherence_point());
      REQUIRE(h.llc.coherence_home == h.llc.coherence_directory.get());
      REQUIRE_FALSE(h.p0.is_coherence_point());
      REQUIRE_FALSE(h.p1.is_coherence_point());
      REQUIRE(h.p0.coherence_home == h.llc.coherence_directory.get());
      REQUIRE(h.p1.coherence_home == h.llc.coherence_directory.get());
      REQUIRE(h.llc.coherence_directory->num_cpus() == 2);
    }

    THEN("Upper-level channels map back to their core")
    {
      REQUIRE_THAT(h.llc.coherence_directory->cpus_for_channel(&h.p0_to_llc), Catch::Matchers::RangeEquals(std::array{0U}));
      REQUIRE_THAT(h.llc.coherence_directory->cpus_for_channel(&h.p1_to_llc), Catch::Matchers::RangeEquals(std::array{1U}));
    }
  }
}

SCENARIO("MESI states follow the protocol across two cores")
{
  GIVEN("A two-core hierarchy and a block address")
  {
    two_core_hierarchy<> h;
    const champsim::address x{0xdeadbe00};
    const auto* dir = h.llc.coherence_directory.get();

    WHEN("Core 0 reads the block")
    {
      h.load(0, x);

      THEN("Core 0 holds it EXCLUSIVE and the directory records one sharer in EXCLUSIVE")
      {
        auto b0 = find_block(h.p0, x);
        REQUIRE(b0.has_value());
        CHECK(b0->state == champsim::coherence_state::EXCLUSIVE);
        CHECK_FALSE(b0->dirty);

        auto bl = find_block(h.llc, x);
        REQUIRE(bl.has_value());
        CHECK(bl->state == champsim::coherence_state::EXCLUSIVE);
        CHECK(bl->sharers == 0b01);
        CHECK(dir->state_of(x) == champsim::coherence_state::EXCLUSIVE);
        CHECK(dir->sharers_of(x) == 0b01);
        CHECK(dir->sim_stats().grants_exclusive == 1);
      }

      AND_WHEN("Core 1 also reads the block")
      {
        h.load(1, x);

        THEN("Both cores hold it SHARED and the directory records two sharers")
        {
          auto b0 = find_block(h.p0, x);
          auto b1 = find_block(h.p1, x);
          REQUIRE(b0.has_value());
          REQUIRE(b1.has_value());
          CHECK(b0->state == champsim::coherence_state::SHARED);
          CHECK(b1->state == champsim::coherence_state::SHARED);

          auto bl = find_block(h.llc, x);
          REQUIRE(bl.has_value());
          CHECK(bl->state == champsim::coherence_state::SHARED);
          CHECK(bl->sharers == 0b11);
          CHECK(dir->sim_stats().grants_shared == 1);
          CHECK(dir->sim_stats().downgrades == 1);
        }

        AND_WHEN("Core 0 writes the block it holds SHARED")
        {
          h.store(0, x);

          THEN("Core 0 upgrades to MODIFIED and core 1 is invalidated")
          {
            auto b0 = find_block(h.p0, x);
            REQUIRE(b0.has_value());
            CHECK(b0->state == champsim::coherence_state::MODIFIED);
            CHECK(b0->dirty);

            CHECK_FALSE(find_block(h.p1, x).has_value());

            auto bl = find_block(h.llc, x);
            REQUIRE(bl.has_value());
            CHECK(bl->state == champsim::coherence_state::MODIFIED);
            CHECK(bl->sharers == 0b01);
            CHECK(dir->sim_stats().upgrades == 1);
            CHECK(dir->sim_stats().invalidations == 1);
          }

          AND_WHEN("Core 1 writes the block")
          {
            h.store(1, x);

            THEN("Core 1 becomes the MODIFIED owner, core 0 is invalidated and its dirty data is absorbed by the coherence point")
            {
              auto b1 = find_block(h.p1, x);
              REQUIRE(b1.has_value());
              CHECK(b1->state == champsim::coherence_state::MODIFIED);
              CHECK(b1->dirty);

              CHECK_FALSE(find_block(h.p0, x).has_value());

              auto bl = find_block(h.llc, x);
              REQUIRE(bl.has_value());
              CHECK(bl->state == champsim::coherence_state::MODIFIED);
              CHECK(bl->sharers == 0b10);
              CHECK(bl->dirty);
              CHECK(dir->sim_stats().grants_modified == 1);
              CHECK(dir->sim_stats().dirty_absorbed >= 1);
            }

            AND_WHEN("Core 0 reads the block back")
            {
              h.load(0, x);

              THEN("The owner is downgraded, both copies are SHARED and clean, and the coherence point is dirty")
              {
                auto b0 = find_block(h.p0, x);
                auto b1 = find_block(h.p1, x);
                REQUIRE(b0.has_value());
                REQUIRE(b1.has_value());
                CHECK(b0->state == champsim::coherence_state::SHARED);
                CHECK(b1->state == champsim::coherence_state::SHARED);
                CHECK_FALSE(b0->dirty);
                CHECK_FALSE(b1->dirty);

                auto bl = find_block(h.llc, x);
                REQUIRE(bl.has_value());
                CHECK(bl->dirty);
                CHECK(bl->state == champsim::coherence_state::SHARED);
                CHECK(bl->sharers == 0b11);
              }
            }
          }
        }
      }
    }

    WHEN("Core 0 writes a block no one holds")
    {
      h.store(0, x);

      THEN("Core 0 is granted MODIFIED directly")
      {
        auto b0 = find_block(h.p0, x);
        REQUIRE(b0.has_value());
        CHECK(b0->state == champsim::coherence_state::MODIFIED);
        CHECK(b0->dirty);
        CHECK(dir->state_of(x) == champsim::coherence_state::MODIFIED);
        CHECK(dir->sharers_of(x) == 0b01);
      }

      AND_WHEN("Core 0 writes it again")
      {
        const auto invalidations_before = dir->sim_stats().invalidations;
        h.store(0, x);

        THEN("No coherence action is needed")
        {
          auto b0 = find_block(h.p0, x);
          REQUIRE(b0.has_value());
          CHECK(b0->state == champsim::coherence_state::MODIFIED);
          CHECK(dir->sim_stats().invalidations == invalidations_before);
          CHECK(dir->sim_stats().upgrades == 0);
        }
      }
    }
  }
}

SCENARIO("Evictions from a private cache release the block in the directory")
{
  GIVEN("A two-core hierarchy where both cores share a block")
  {
    two_core_hierarchy<> h;
    const champsim::address x{0xdeadbe00};
    const champsim::address y{0xcafeba00};
    const auto* dir = h.llc.coherence_directory.get();

    h.load(0, x);
    h.load(1, x);
    REQUIRE(dir->sharers_of(x) == 0b11);

    WHEN("Core 1 loads a different block, evicting its only way")
    {
      h.load(1, y);

      THEN("Core 1 is removed from the sharer set and core 0 keeps its SHARED copy")
      {
        CHECK_FALSE(find_block(h.p1, x).has_value());
        CHECK(dir->sharers_of(x) == 0b01);
        CHECK(dir->state_of(x) == champsim::coherence_state::SHARED);
        CHECK(dir->sim_stats().releases >= 1);

        auto bl = find_block(h.llc, x);
        REQUIRE(bl.has_value());
        CHECK(bl->sharers == 0b01);
      }

      AND_WHEN("Core 0 also evicts the block")
      {
        h.load(0, y);

        THEN("The directory forgets the block entirely")
        {
          CHECK(dir->sharers_of(x) == 0);
          CHECK(dir->state_of(x) == champsim::coherence_state::INVALID);

          auto bl = find_block(h.llc, x);
          REQUIRE(bl.has_value());
          CHECK(bl->sharers == 0);
          CHECK(bl->state == champsim::coherence_state::INVALID);
        }
      }
    }
  }

  GIVEN("A two-core hierarchy where core 0 has written a block")
  {
    two_core_hierarchy<> h;
    const champsim::address x{0xdeadbe00};
    const champsim::address y{0xcafeba00};
    const auto* dir = h.llc.coherence_directory.get();

    h.store(0, x);
    REQUIRE(dir->state_of(x) == champsim::coherence_state::MODIFIED);

    WHEN("Core 0 evicts the dirty block")
    {
      h.load(0, y);

      THEN("The writeback lands at the coherence point, which becomes dirty, and the directory drops the sharer")
      {
        auto bl = find_block(h.llc, x);
        REQUIRE(bl.has_value());
        CHECK(bl->dirty);
        CHECK(dir->sharers_of(x) == 0);
        CHECK(dir->state_of(x) == champsim::coherence_state::INVALID);
      }
    }
  }
}

SCENARIO("A fill that is invalidated in flight is delivered but not installed")
{
  GIVEN("A two-core hierarchy whose DRAM holds responses until released, with a core 0 load and a core 1 store to the same block outstanding")
  {
    two_core_hierarchy<release_MRC> h;
    const champsim::address x{0xdeadbe00};
    const auto* dir = h.llc.coherence_directory.get();

    // Core 0's load misses all the way to DRAM, which holds it
    REQUIRE(h.core0_loads.issue(two_core_hierarchy<release_MRC>::make_request(x, 0, access_type::LOAD, 1)));
    h.run(10);
    REQUIRE(h.p0.coherence_holds(x));
    REQUIRE_FALSE(find_block(h.p0, x).has_value());
    REQUIRE(h.dram.packet_count() == 1);

    // Core 1's store merges onto the outstanding LLC miss for the same block
    REQUIRE(h.core1_stores.issue(two_core_hierarchy<release_MRC>::make_request(x, 1, access_type::WRITE, 2)));
    h.run(10);
    REQUIRE(h.p1.coherence_holds(x));
    REQUIRE(h.dram.packet_count() == 1);

    WHEN("DRAM releases the block and one fill answers both cores")
    {
      h.dram.release_all();
      h.run(60);

      // The merged fill is granted to each requester in turn as a read-for-ownership. Whichever core is served second invalidates
      // the first core's copy while it is still in flight, so exactly one core ends up holding the block.
      THEN("Exactly one core owns the block MODIFIED and the directory agrees")
      {
        auto b0 = find_block(h.p0, x);
        auto b1 = find_block(h.p1, x);
        REQUIRE(b0.has_value() != b1.has_value());
        const auto& owner = b0.has_value() ? *b0 : *b1;
        CHECK(owner.state == champsim::coherence_state::MODIFIED);
        CHECK(dir->state_of(x) == champsim::coherence_state::MODIFIED);
        CHECK(dir->sharers_of(x) == (b0.has_value() ? 0b01U : 0b10U));
      }

      THEN("The other core's request completes, but its copy, granted and then invalidated in flight, is never installed")
      {
        REQUIRE_FALSE(std::empty(h.core0_loads.packets));
        REQUIRE_FALSE(std::empty(h.core1_stores.packets));
        CHECK(h.core0_loads.packets.back().return_time > 0);
        CHECK(h.core1_stores.packets.back().return_time > 0);
        CHECK(dir->sim_stats().dropped_fills == 1);
        CHECK(dir->sim_stats().invalidations == 1);
      }
    }
  }
}

SCENARIO("Caches outside a coherence domain are unaffected")
{
  GIVEN("A lone cache with no coherence home")
  {
    constexpr uint64_t hit_latency = 1;
    constexpr uint64_t fill_latency = 1;
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_ul;
    CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                  .name("440-lone")
                  .sets(1)
                  .ways(1)
                  .upper_levels({&mock_ul.queues})
                  .lower_level(&mock_ll.queues)
                  .hit_latency(hit_latency)
                  .fill_latency(fill_latency)};

    std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_ul}};
    for (auto* elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    REQUIRE_FALSE(uut.is_coherence_point());
    REQUIRE(uut.coherence_home == nullptr);

    WHEN("A block is read")
    {
      const champsim::address x{0xdeadbe00};
      auto req = two_core_hierarchy<>::make_request(x, 0, access_type::LOAD, 1);
      REQUIRE(mock_ul.issue(req));
      for (int i = 0; i < 20; ++i) {
        for (auto* elem : elements) {
          elem->_operate();
        }
      }

      THEN("It is installed as a clean EXCLUSIVE copy with no sharers")
      {
        auto b = find_block(uut, x);
        REQUIRE(b.has_value());
        CHECK(b->state == champsim::coherence_state::EXCLUSIVE);
        CHECK(b->sharers == 0);
        CHECK_FALSE(b->dirty);
      }
    }
  }
}
