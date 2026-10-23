#include <catch.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <random>

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


namespace {
template <std::size_t N, bool WITH_L2 = false>
struct validation_hierarchy {
  do_nothing_MRC dram;
  std::array<champsim::channel, N> private_to_home{}, l1_to_l2{};
  std::array<to_rq_MRP, N> reads{};
  std::array<to_wq_MRP, N> writes{};
  std::unique_ptr<CACHE> home;
  std::array<std::unique_ptr<CACHE>, N> l1{}, l2{};
  std::vector<champsim::operable*> elements;
  uint64_t id = 1;

  explicit validation_hierarchy(uint64_t l2_fill_latency = 1) {
    std::vector<champsim::channel*> upper;
    for (auto& ch : private_to_home) upper.push_back(&ch);
    home = std::make_unique<CACHE>(champsim::cache_builder{champsim::defaults::default_llc}
      .name("validation-home").sets(1).ways(8).upper_levels(std::move(upper))
      .lower_level(&dram.queues).hit_latency(1).fill_latency(1));
    elements.push_back(home.get());
    std::vector<CACHE*> caches{home.get()};
    std::vector<champsim::coherence::entry_point> entries;
    for (std::size_t i = 0; i < N; ++i) {
      if constexpr (WITH_L2) {
        l2[i] = std::make_unique<CACHE>(champsim::cache_builder{champsim::defaults::default_l2c}
          .name("validation-l2-" + std::to_string(i)).sets(1).ways(2)
          .upper_levels({&l1_to_l2[i]}).lower_level(&private_to_home[i]).hit_latency(1).fill_latency(l2_fill_latency));
        elements.push_back(l2[i].get());
        caches.push_back(l2[i].get());
      }
      l1[i] = std::make_unique<CACHE>(champsim::cache_builder{champsim::defaults::default_l1d}
        .name("validation-l1-" + std::to_string(i)).sets(1).ways(1)
        .upper_levels({&reads[i].queues, &writes[i].queues})
        .lower_level(WITH_L2 ? &l1_to_l2[i] : &private_to_home[i]).hit_latency(1).fill_latency(1));
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
      element->initialize(); element->warmup = false; element->begin_phase();
    }
  }
  void run(int cycles = 80) {
    for (int n = 0; n < cycles; ++n) for (auto* element : elements) element->_operate();
  }
  void access(uint32_t cpu, champsim::address addr, access_type type = access_type::LOAD) {
    auto req = two_core_hierarchy<>::make_request(addr, cpu, type, id++);
    REQUIRE((type == access_type::WRITE ? writes[cpu].issue(req) : reads[cpu].issue(req)));
    run();
  }
  void check_settled_invariants(champsim::address addr) {
    uint64_t actual_mask = 0;
    uint64_t exclusive_mask = 0;
    uint64_t modified_mask = 0;
    for (std::size_t cpu = 0; cpu < N; ++cpu) {
      for (const CACHE* cache : {l1[cpu].get(), l2[cpu].get()}) {
        if (!cache) continue;
        auto block = find_block(*cache, addr);
        if (!block) continue;
        const auto mask = champsim::coherence::directory::bit(static_cast<uint32_t>(cpu));
        actual_mask |= mask;
        if (block->state == champsim::coherence_state::EXCLUSIVE) exclusive_mask |= mask;
        if (block->state == champsim::coherence_state::MODIFIED) modified_mask |= mask;
        CHECK(block->state != champsim::coherence_state::INVALID);
        if (block->dirty) CHECK(block->state == champsim::coherence_state::MODIFIED);
      }
    }
    auto* dir = home->coherence_directory.get();
    CHECK(dir->sharers_of(addr) == actual_mask);
    CHECK(champsim::coherence::directory::popcount(modified_mask) <= 1);
    CHECK(champsim::coherence::directory::popcount(exclusive_mask) <= 1);
    if (exclusive_mask) CHECK(actual_mask == exclusive_mask);
    if (modified_mask) CHECK(actual_mask == modified_mask);
  }
};
}

TEST_CASE("Validation: exclusive write keeps exact directory state consistent", "[mesi-validation]") {
  two_core_hierarchy<> h;
  const champsim::address x{0x1000};
  h.load(0, x);
  h.store(0, x);
  REQUIRE(find_block(h.p0, x).has_value());
  CHECK(find_block(h.p0, x)->state == champsim::coherence_state::MODIFIED);
  CHECK(find_block(h.p0, x)->dirty);
  CHECK(h.llc.coherence_directory->sim_stats().invalidations == 0);
  // The documented research interface promises an exact view of upper copies.
  CHECK(h.llc.coherence_directory->state_of(x) == champsim::coherence_state::MODIFIED);
  REQUIRE(find_block(h.llc, x).has_value());
  CHECK(find_block(h.llc, x)->state == champsim::coherence_state::MODIFIED);
}

TEST_CASE("Validation: remote read after exclusive write preserves dirty obligation", "[mesi-validation]") {
  two_core_hierarchy<> h;
  const champsim::address x{0x1000};
  h.load(0, x); h.store(0, x); h.load(1, x);
  REQUIRE(find_block(h.p0, x).has_value());
  REQUIRE(find_block(h.p1, x).has_value());
  CHECK(find_block(h.p0, x)->state == champsim::coherence_state::SHARED);
  CHECK(find_block(h.p1, x)->state == champsim::coherence_state::SHARED);
  CHECK_FALSE(find_block(h.p0, x)->dirty);
  CHECK(find_block(h.llc, x)->dirty);
}

TEST_CASE("Validation: different bytes of the same cache line share ownership", "[mesi-validation]") {
  two_core_hierarchy<> h;
  const champsim::address x{0x1000}, y{0x1008};
  h.load(0, x); h.load(1, y); h.store(0, x);
  CHECK_FALSE(find_block(h.p1, y).has_value());
  CHECK(h.llc.coherence_directory->sharers_of(y) == 1);
  CHECK(h.llc.coherence_directory->sim_stats().invalidations == 1);
}

TEST_CASE("Validation: unrelated lines do not invalidate each other", "[mesi-validation]") {
  validation_hierarchy<2> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x, access_type::WRITE); h.access(1, y, access_type::WRITE);
  CHECK(find_block(*h.l1[0], x).has_value());
  CHECK(find_block(*h.l1[1], y).has_value());
  CHECK(h.home->coherence_directory->sim_stats().invalidations == 0);
  h.check_settled_invariants(x); h.check_settled_invariants(y);
}

TEST_CASE("Validation: four readers then writer invalidates all other cores", "[mesi-validation]") {
  validation_hierarchy<4> h;
  const champsim::address x{0x1000};
  for (uint32_t cpu = 0; cpu < 4; ++cpu) h.access(cpu, x);
  CHECK(h.home->coherence_directory->sharers_of(x) == 15);
  h.access(2, x, access_type::WRITE);
  for (uint32_t cpu : {0U, 1U, 3U}) CHECK_FALSE(find_block(*h.l1[cpu], x).has_value());
  CHECK(h.home->coherence_directory->sharers_of(x) == 4);
  CHECK(h.home->coherence_directory->sim_stats().invalidations == 3);
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: private L1 eviction retains sharer while L2 holds copy", "[mesi-validation]") {
  validation_hierarchy<2, true> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x); h.access(1, x); h.access(0, y);
  CHECK_FALSE(find_block(*h.l1[0], x).has_value());
  CHECK(find_block(*h.l2[0], x).has_value());
  CHECK(h.home->coherence_directory->sharers_of(x) == 3);
  h.check_settled_invariants(x);
  h.access(1, x, access_type::WRITE);
  CHECK_FALSE(find_block(*h.l2[0], x).has_value());
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: RFO hitting a shared private L2 obtains ownership", "[mesi-validation]") {
  validation_hierarchy<2, true> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x); h.access(1, x); h.access(0, y);
  REQUIRE(find_block(*h.l2[0], x).has_value());
  REQUIRE(find_block(*h.l1[1], x).has_value());
  // An RFO is the downstream request created by a store miss at a private level.
  h.access(0, x, access_type::RFO);
  CHECK_FALSE(find_block(*h.l1[1], x).has_value());
  CHECK_FALSE(find_block(*h.l2[1], x).has_value());
  CHECK(h.home->coherence_directory->sharers_of(x) == 1);
  REQUIRE(find_block(*h.l1[0], x).has_value());
  CHECK(find_block(*h.l1[0], x)->state == champsim::coherence_state::MODIFIED);
}

TEST_CASE("Validation: real store miss hitting shared L2 invalidates remote copies", "[mesi-validation]") {
  validation_hierarchy<2, true> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x); h.access(1, x); h.access(0, y);
  h.access(0, x, access_type::WRITE);
  CHECK_FALSE(find_block(*h.l1[1], x).has_value());
  CHECK_FALSE(find_block(*h.l2[1], x).has_value());
  REQUIRE(find_block(*h.l1[0], x).has_value());
  CHECK(find_block(*h.l1[0], x)->dirty);
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: multi-level silent write reports modified state to research interface", "[mesi-validation]") {
  validation_hierarchy<2, true> h;
  const champsim::address x{0x1000};
  h.access(0, x); h.access(0, x, access_type::WRITE);
  CHECK(h.home->coherence_directory->state_of(x) == champsim::coherence_state::MODIFIED);
  CHECK(find_block(*h.l1[0], x)->state == champsim::coherence_state::MODIFIED);
  CHECK(find_block(*h.l2[0], x)->state == champsim::coherence_state::MODIFIED);
  CHECK(find_block(*h.home, x)->state == champsim::coherence_state::MODIFIED);
  CHECK(h.home->coherence_directory->sim_stats().upgrades == 0);
  CHECK(h.home->coherence_directory->sim_stats().invalidations == 0);
  h.access(1, x);
  CHECK(find_block(*h.home, x)->dirty);
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: multi-level alternating writers leave one owner", "[mesi-validation]") {
  validation_hierarchy<4, true> h;
  const champsim::address x{0x1000};
  for (int round = 0; round < 3; ++round) {
    for (uint32_t cpu = 0; cpu < 4; ++cpu) {
      h.access(cpu, x, access_type::WRITE);
      h.check_settled_invariants(x);
      CHECK(h.home->coherence_directory->sharers_of(x) == (uint64_t{1} << cpu));
    }
  }
}

TEST_CASE("Validation: deterministic mixed accesses preserve settled ownership", "[mesi-validation]") {
  validation_hierarchy<4, true> h;
  std::mt19937 random{20261013};
  std::array<champsim::address, 4> blocks{{champsim::address{0x1000}, champsim::address{0x2000},
                                      champsim::address{0x3000}, champsim::address{0x4000}}};
  for (int step = 0; step < 100; ++step) {
    const auto cpu = static_cast<uint32_t>(random() % 4);
    auto address = blocks[random() % blocks.size()];
    const auto type = random() % 2 ? access_type::LOAD : access_type::WRITE;
    INFO("step=" << step << " core=" << cpu << " address=" << address.to<uint64_t>() << " operation=" << static_cast<int>(type));
    h.access(cpu, address, type);
    for (auto block : blocks) h.check_settled_invariants(block);
  }
}

TEST_CASE("Validation: merged simultaneous reads end shared", "[mesi-validation]") {
  two_core_hierarchy<release_MRC> h;
  const champsim::address x{0x1000};
  REQUIRE(h.core0_loads.issue(two_core_hierarchy<>::make_request(x, 0, access_type::LOAD, 1)));
  REQUIRE(h.core1_loads.issue(two_core_hierarchy<>::make_request(x, 1, access_type::LOAD, 2)));
  h.run(20); h.dram.release_all(); h.run(80);
  REQUIRE(find_block(h.p0, x).has_value());
  REQUIRE(find_block(h.p1, x).has_value());
  CHECK(find_block(h.p0, x)->state == champsim::coherence_state::SHARED);
  CHECK(find_block(h.p1, x)->state == champsim::coherence_state::SHARED);
  CHECK(h.llc.coherence_directory->sharers_of(x) == 3);
}

TEST_CASE("Validation: simultaneous writes complete with one modified owner", "[mesi-validation]") {
  two_core_hierarchy<release_MRC> h;
  const champsim::address x{0x1000};
  REQUIRE(h.core0_stores.issue(two_core_hierarchy<>::make_request(x, 0, access_type::WRITE, 1)));
  REQUIRE(h.core1_stores.issue(two_core_hierarchy<>::make_request(x, 1, access_type::WRITE, 2)));
  h.run(20); h.dram.release_all(); h.run(80);
  auto a = find_block(h.p0, x), b = find_block(h.p1, x);
  CHECK(a.has_value() != b.has_value());
  CHECK(h.core0_stores.packets.back().return_time > 0);
  CHECK(h.core1_stores.packets.back().return_time > 0);
  CHECK(champsim::coherence::directory::popcount(h.llc.coherence_directory->sharers_of(x)) == 1);
}

TEST_CASE("Validation: delayed old writeback cannot steal ownership from a newer writer", "[mesi-validation]") {
  validation_hierarchy<2, true> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x, access_type::WRITE);
  auto req = two_core_hierarchy<>::make_request(y, 0, access_type::LOAD, h.id++);
  REQUIRE(h.reads[0].issue(req));
  // Pause delivery of the actual writeback emitted when L1 replaces dirty X.
  std::optional<champsim::channel::request_type> delayed;
  for (int cycle = 0; cycle < 100 && !delayed; ++cycle) {
    h.run(1);
    auto& queue = h.l1_to_l2[0].WQ;
    auto it = std::find_if(queue.begin(), queue.end(), [x](const auto& packet) { return packet.address == x; });
    if (it != queue.end()) { delayed = *it; queue.erase(it); }
  }
  REQUIRE(delayed.has_value());
  h.run();
  h.access(1, x, access_type::WRITE);
  REQUIRE(h.home->coherence_directory->sharers_of(x) == 2);
  REQUIRE(find_block(*h.l1[1], x).has_value());
  REQUIRE(h.l1_to_l2[0].add_wq(*delayed));
  h.run();
  CHECK(h.home->coherence_directory->sharers_of(x) == 2);
  CHECK(find_block(*h.l1[1], x).has_value());
  CHECK_FALSE(find_block(*h.l2[0], x).has_value());
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: RFO hitting exclusive private copy grants modified permission", "[mesi-validation]") {
  two_core_hierarchy<> h;
  const champsim::address x{0x1000};
  h.load(0, x);
  REQUIRE(h.core0_loads.issue(two_core_hierarchy<>::make_request(x, 0, access_type::RFO, h.next_id++)));
  h.run();
  CHECK(find_block(h.p0, x)->state == champsim::coherence_state::MODIFIED);
}

namespace {
template <typename H>
champsim::channel::request_type pause_l1_writeback(H& h, champsim::address x, champsim::address y) {
  REQUIRE(h.reads[0].issue(two_core_hierarchy<>::make_request(y, 0, access_type::LOAD, h.id++)));
  for (int cycle = 0; cycle < 100; ++cycle) {
    h.run(1);
    auto& queue = h.l1_to_l2[0].WQ;
    auto it = std::find_if(queue.begin(), queue.end(), [x](const auto& packet) { return packet.address == x; });
    if (it != queue.end()) {
      auto delayed = *it;
      queue.erase(it);
      REQUIRE(delayed.is_writeback);
      REQUIRE(delayed.coherence_writeback_id != 0);
      REQUIRE(h.home->coherence_directory->writeback_active(delayed.coherence_writeback_id));
      return delayed;
    }
  }
  FAIL("Expected an L1 dirty eviction writeback");
  return {};
}
}

TEST_CASE("Validation: valid writeback fills an absent L2 without requesting ownership", "[mesi-validation]") {
  validation_hierarchy<2, true> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x, access_type::WRITE);
  REQUIRE(find_block(*h.l1[0], x).has_value());
  h.l2[0]->invalidate_entry(x);
  const auto upgrades = h.home->coherence_directory->sim_stats().upgrades;
  h.access(0, y);
  REQUIRE(find_block(*h.l2[0], x).has_value());
  CHECK(find_block(*h.l2[0], x)->dirty);
  CHECK(find_block(*h.l2[0], x)->state == champsim::coherence_state::MODIFIED);
  CHECK(h.home->coherence_directory->sim_stats().upgrades == upgrades);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 0);
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: final dirty eviction reaches LLC and retires its tracking record", "[mesi-validation]") {
  validation_hierarchy<2> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x, access_type::WRITE);
  REQUIRE(h.reads[0].issue(two_core_hierarchy<>::make_request(y, 0, access_type::LOAD, h.id++)));
  std::optional<champsim::channel::request_type> delayed;
  for (int cycle = 0; cycle < 100 && !delayed; ++cycle) {
    h.run(1);
    auto& queue = h.private_to_home[0].WQ;
    auto it = std::find_if(queue.begin(), queue.end(), [x](const auto& packet) { return packet.address == x; });
    if (it != queue.end()) { delayed = *it; queue.erase(it); }
  }
  REQUIRE(delayed.has_value());
  REQUIRE(delayed->is_writeback);
  h.run();
  CHECK_FALSE(find_block(*h.l1[0], x).has_value());
  // A dirty copy still exists in transport, so it must remain accounted for.
  CHECK(h.home->coherence_directory->sharers_of(x) == 1);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 1);
  REQUIRE(h.private_to_home[0].add_wq(*delayed));
  h.run();
  REQUIRE(find_block(*h.home, x).has_value());
  CHECK(find_block(*h.home, x)->dirty);
  CHECK(h.home->coherence_directory->sharers_of(x) == 0);
  CHECK(h.home->coherence_directory->state_of(x) == champsim::coherence_state::INVALID);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 0);
}

TEST_CASE("Validation: remote read absorbs delayed dirty writeback without losing shared copies", "[mesi-validation]") {
  validation_hierarchy<2, true> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x, access_type::WRITE);
  auto delayed = pause_l1_writeback(h, x, y);
  h.run();
  h.access(1, x);
  REQUIRE(find_block(*h.home, x).has_value());
  CHECK(find_block(*h.home, x)->dirty);
  CHECK_FALSE(h.home->coherence_directory->writeback_active(delayed.coherence_writeback_id));
  const auto upgrades = h.home->coherence_directory->sim_stats().upgrades;
  REQUIRE(h.l1_to_l2[0].add_wq(delayed));
  h.run();
  CHECK(h.home->coherence_directory->sim_stats().upgrades == upgrades);
  CHECK(h.home->coherence_directory->sim_stats().dropped_writebacks == 1);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 0);
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: obsolete writeback stays obsolete after ownership returns to its core", "[mesi-validation]") {
  validation_hierarchy<2, true> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x, access_type::WRITE);
  auto delayed = pause_l1_writeback(h, x, y);
  h.run();
  h.access(1, x, access_type::WRITE);
  h.access(0, x, access_type::WRITE);
  const auto upgrades = h.home->coherence_directory->sim_stats().upgrades;
  REQUIRE(h.l1_to_l2[0].add_wq(delayed));
  h.run();
  CHECK(h.home->coherence_directory->sharers_of(x) == 1);
  CHECK(h.home->coherence_directory->sim_stats().upgrades == upgrades);
  CHECK(h.home->coherence_directory->sim_stats().dropped_writebacks == 1);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 0);
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: writeback fill invalidated before installation cannot resurrect old owner", "[mesi-validation]") {
  validation_hierarchy<2, true> h{20};
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x, access_type::WRITE);
  REQUIRE(find_block(*h.l1[0], x).has_value());
  h.l2[0]->invalidate_entry(x);
  auto delayed = pause_l1_writeback(h, x, y);
  REQUIRE(h.l1_to_l2[0].add_wq(delayed));
  h.run(5);
  CHECK_FALSE(find_block(*h.l2[0], x).has_value());
  CHECK(h.l2[0]->coherence_holds(x));
  h.access(1, x, access_type::WRITE);
  CHECK_FALSE(find_block(*h.l2[0], x).has_value());
  REQUIRE(find_block(*h.l1[1], x).has_value());
  CHECK(h.home->coherence_directory->sharers_of(x) == 2);
  CHECK(h.home->coherence_directory->sim_stats().dropped_writebacks == 1);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 0);
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: blocked writeback queue preserves dirty victim without leaking tokens", "[mesi-validation]") {
  validation_hierarchy<2> h;
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x, access_type::WRITE);
  h.private_to_home[0] = champsim::channel{8, 8, 0, champsim::data::bits{6}, false};
  h.access(0, y);
  REQUIRE(find_block(*h.l1[0], x).has_value());
  CHECK(find_block(*h.l1[0], x)->dirty);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 0);
  CHECK(h.reads[0].packets.back().return_time == 0);
  h.private_to_home[0] = champsim::channel{};
  h.run();
  CHECK(h.reads[0].packets.back().return_time > 0);
  CHECK(find_block(*h.home, x)->dirty);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 0);
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: RFO grants permission without dirtying otherwise clean data", "[mesi-validation]") {
  two_core_hierarchy<> h;
  const champsim::address x{0x1000};
  h.load(0, x); h.load(1, x);
  REQUIRE(h.core0_loads.issue(two_core_hierarchy<>::make_request(x, 0, access_type::RFO, h.next_id++)));
  h.run();
  REQUIRE(find_block(h.p0, x).has_value());
  CHECK(find_block(h.p0, x)->state == champsim::coherence_state::MODIFIED);
  CHECK_FALSE(find_block(h.p0, x)->dirty);
  CHECK_FALSE(find_block(h.llc, x)->dirty);
  CHECK_FALSE(find_block(h.p1, x).has_value());
}

TEST_CASE("Validation: non-returning CPU stores are distinct from eviction writebacks", "[mesi-validation]") {
  validation_hierarchy<2, true> h;
  const champsim::address x{0x1000};
  h.access(0, x); h.access(1, x);
  auto req = two_core_hierarchy<>::make_request(x, 0, access_type::WRITE, h.id++);
  req.response_requested = false; // Same request setting used by CacheBus::issue_write.
  REQUIRE_FALSE(req.is_writeback);
  REQUIRE(h.writes[0].issue(req));
  h.run();
  REQUIRE(find_block(*h.l1[0], x).has_value());
  CHECK(find_block(*h.l1[0], x)->dirty);
  CHECK_FALSE(find_block(*h.l1[1], x).has_value());
  CHECK(h.home->coherence_directory->state_of(x) == champsim::coherence_state::MODIFIED);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 0);
  h.check_settled_invariants(x);
}

TEST_CASE("Validation: demand merging with writeback fill preserves dirty data and retires token", "[mesi-validation]") {
  validation_hierarchy<2, true> h{20};
  const champsim::address x{0x1000}, y{0x2000};
  h.access(0, x, access_type::WRITE);
  h.l2[0]->invalidate_entry(x);
  auto delayed = pause_l1_writeback(h, x, y);
  REQUIRE(h.l1_to_l2[0].add_wq(delayed));
  h.run(5);
  REQUIRE(h.l2[0]->coherence_holds(x));
  REQUIRE_FALSE(find_block(*h.l2[0], x).has_value());
  SECTION("Read merges into the outstanding writeback") {
    h.access(0, x);
  }
  SECTION("Ownership request merges into the outstanding writeback") {
    h.access(0, x, access_type::RFO);
  }
  REQUIRE(find_block(*h.l2[0], x).has_value());
  CHECK(find_block(*h.l2[0], x)->dirty);
  CHECK(find_block(*h.l2[0], x)->data == delayed.data);
  REQUIRE(find_block(*h.l1[0], x).has_value());
  CHECK(find_block(*h.l1[0], x)->data == delayed.data);
  CHECK(h.home->coherence_directory->num_pending_writebacks() == 0);
  h.check_settled_invariants(x);
}
