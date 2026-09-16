/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "coherence.h"

#include <algorithm>
#include <bitset>
#include <cassert>
#include <iterator>
#include <fmt/core.h>

#include "cache.h"
#include "channel.h"
#include "environment.h"
#include "ooo_cpu.h"

namespace
{
constexpr uint32_t MAX_TRACKED_CPUS = 64; // one bit per core in the sharer mask

template <typename F>
void for_each_set_bit(uint64_t mask, F&& func)
{
  for (uint32_t i = 0; i < MAX_TRACKED_CPUS && (mask >> i) != 0; ++i) {
    if (((mask >> i) & 1U) != 0) {
      func(i);
    }
  }
}
} // namespace

namespace champsim::coherence
{
directory::directory(CACHE* home_) : home(home_) {}

void directory::rebind_home(CACHE* home_) { home = home_; }

std::size_t directory::popcount(uint64_t mask) { return std::bitset<MAX_TRACKED_CPUS>{mask}.count(); }

void directory::add_member(uint32_t cpu, CACHE* cache)
{
  assert(cpu < MAX_TRACKED_CPUS);
  if (std::size(members) <= cpu) {
    members.resize(cpu + 1);
  }
  auto& group = members.at(cpu);
  if (std::find(std::begin(group), std::end(group), cache) == std::end(group)) {
    group.push_back(cache);
  }
}

uint64_t directory::key(champsim::address addr) const { return addr.slice_upper(home->OFFSET_BITS).to<uint64_t>(); }

bool directory::holds(uint32_t cpu, champsim::address addr) const
{
  if (cpu >= std::size(members)) {
    return false;
  }
  const auto& group = members.at(cpu);
  return std::any_of(std::begin(group), std::end(group), [addr](const CACHE* cache) { return cache->coherence_holds(addr); });
}

bool directory::invalidate_cpu(uint32_t cpu, champsim::address addr)
{
  bool dirty = false;
  if (cpu < std::size(members)) {
    for (CACHE* cache : members.at(cpu)) {
      dirty |= cache->coherence_invalidate(addr);
    }
  }
  return dirty;
}

bool directory::downgrade_cpu(uint32_t cpu, champsim::address addr)
{
  bool dirty = false;
  if (cpu < std::size(members)) {
    for (CACHE* cache : members.at(cpu)) {
      dirty |= cache->coherence_downgrade(addr);
    }
  }
  return dirty;
}

void directory::refresh_home(champsim::address addr, const entry* e) const
{
  home->coherence_refresh(addr, (e != nullptr) ? e->state : coherence_state::INVALID, (e != nullptr) ? e->sharers : 0);
}

auto directory::request(champsim::address addr, uint32_t cpu, access_type type) -> grant
{
  assert(cpu < MAX_TRACKED_CPUS);
  auto& e = entries[key(addr)];
  const auto me = bit(cpu);
  const auto others = e.sharers & ~me;
  bool absorbed = false;

  if (type == access_type::RFO || type == access_type::WRITE) {
    // Read for ownership: every other copy is destroyed and the requester becomes the sole, modified owner
    for_each_set_bit(others, [&](uint32_t other) {
      absorbed |= invalidate_cpu(other, addr);
      ++m_sim_stats.invalidations;
    });
    e.sharers = me;
    e.state = coherence_state::MODIFIED;
    ++m_sim_stats.grants_modified;
  } else if (others != 0) {
    // Read while another core holds the block: the owner (if any) is demoted and everyone ends up SHARED
    for_each_set_bit(others, [&](uint32_t other) {
      absorbed |= downgrade_cpu(other, addr);
      ++m_sim_stats.downgrades;
    });
    e.sharers |= me;
    e.state = coherence_state::SHARED;
    ++m_sim_stats.grants_shared;
  } else if (e.sharers == 0) {
    // Read of a block no core holds: the requester becomes the exclusive owner
    e.sharers = me;
    e.state = coherence_state::EXCLUSIVE;
    ++m_sim_stats.grants_exclusive;
  } else {
    // Only the requesting core already holds this block (one of its private levels is refilling it): the state is unchanged
    if (e.state == coherence_state::INVALID) {
      e.state = coherence_state::EXCLUSIVE;
    }
  }

  if (absorbed) {
    ++m_sim_stats.dirty_absorbed;
  }

  refresh_home(addr, &e);
  return {e.state, absorbed};
}

bool directory::upgrade(champsim::address addr, uint32_t cpu)
{
  assert(cpu < MAX_TRACKED_CPUS);
  auto& e = entries[key(addr)];
  const auto me = bit(cpu);
  bool absorbed = false;

  for_each_set_bit(e.sharers & ~me, [&](uint32_t other) {
    absorbed |= invalidate_cpu(other, addr);
    ++m_sim_stats.invalidations;
  });

  e.sharers = me;
  e.state = coherence_state::MODIFIED;
  ++m_sim_stats.upgrades;

  // Every copy the requesting core holds (for example, both its L1D and L2C) becomes MODIFIED
  if (cpu < std::size(members)) {
    for (CACHE* cache : members.at(cpu)) {
      cache->coherence_set_modified(addr);
    }
  }

  if (absorbed) {
    ++m_sim_stats.dirty_absorbed;
  }

  refresh_home(addr, &e);
  return absorbed;
}

void directory::release(champsim::address addr)
{
  auto it = entries.find(key(addr));
  if (it == std::end(entries)) {
    refresh_home(addr, nullptr);
    return;
  }

  auto& e = it->second;
  const auto before = e.sharers;
  for_each_set_bit(before, [&](uint32_t cpu) {
    if (!holds(cpu, addr)) {
      e.sharers &= ~bit(cpu);
    }
  });

  if (e.sharers != before) {
    ++m_sim_stats.releases;
  }

  if (e.sharers == 0) {
    entries.erase(it);
    refresh_home(addr, nullptr);
    return;
  }

  // A lone remaining sharer keeps its state: MESI has no silent SHARED -> EXCLUSIVE promotion
  refresh_home(addr, &e);
}

coherence_state directory::state_of(champsim::address addr) const
{
  auto it = entries.find(key(addr));
  return (it != std::end(entries)) ? it->second.state : coherence_state::INVALID;
}

uint64_t directory::sharers_of(champsim::address addr) const
{
  auto it = entries.find(key(addr));
  return (it != std::end(entries)) ? it->second.sharers : 0;
}

std::vector<uint32_t> directory::cpus_for_channel(const champsim::channel* ch) const
{
  std::vector<uint32_t> retval{};
  for (uint32_t cpu = 0; cpu < std::size(members); ++cpu) {
    const auto& group = members.at(cpu);
    if (std::any_of(std::begin(group), std::end(group), [ch](const CACHE* cache) { return cache->lower_level == ch; })) {
      retval.push_back(cpu);
    }
  }
  return retval;
}

std::vector<uint32_t> directory::cpus_for_return_queue(const std::deque<champsim::channel::response_type>* queue) const
{
  // A cache's responses to an upper level are pushed into that upper level's lower_level->returned deque
  std::vector<uint32_t> retval{};
  for (uint32_t cpu = 0; cpu < std::size(members); ++cpu) {
    const auto& group = members.at(cpu);
    if (std::any_of(std::begin(group), std::end(group),
                    [queue](const CACHE* cache) { return cache->lower_level != nullptr && &cache->lower_level->returned == queue; })) {
      retval.push_back(cpu);
    }
  }
  return retval;
}

void directory::begin_phase() { m_sim_stats = directory_stats{}; }

void directory::end_phase() { m_roi_stats = m_sim_stats; }

void directory::print_stats() const
{
  const auto& s = m_roi_stats;
  fmt::print("{} COHERENCE DIRECTORY  cores: {}  tracked blocks: {}\n", home->NAME, std::size(members), std::size(entries));
  fmt::print("{} COHERENCE GRANTS  E: {}  S: {}  M: {}  UPGRADES: {}\n", home->NAME, s.grants_exclusive, s.grants_shared, s.grants_modified, s.upgrades);
  fmt::print("{} COHERENCE INVALIDATIONS: {}  DOWNGRADES: {}  DIRTY ABSORBED: {}  RELEASES: {}  DROPPED FILLS: {}\n", home->NAME, s.invalidations,
             s.downgrades, s.dirty_absorbed, s.releases, s.dropped_fills);
}

void resolve(const std::vector<CACHE*>& caches, const std::vector<entry_point>& entry_points)
{
  auto owner_of = [&caches](const champsim::channel* ch) -> CACHE* {
    if (ch == nullptr) {
      return nullptr;
    }
    auto found = std::find_if(std::begin(caches), std::end(caches), [ch](const CACHE* cache) {
      return std::find(std::begin(cache->upper_levels), std::end(cache->upper_levels), ch) != std::end(cache->upper_levels);
    });
    return (found != std::end(caches)) ? *found : nullptr;
  };

  for (const auto& ep : entry_points) {
    // Walk from the core's L1 down to the last cache before DRAM
    std::vector<CACHE*> path{};
    for (CACHE* cache = owner_of(ep.channel); cache != nullptr; cache = owner_of(cache->lower_level)) {
      if (std::find(std::begin(path), std::end(path), cache) != std::end(path)) {
        break; // malformed topology; do not loop forever
      }
      path.push_back(cache);
    }

    if (std::empty(path)) {
      continue;
    }

    CACHE* home = path.back();
    if (home->coherence_directory == nullptr) {
      home->coherence_directory = std::make_unique<directory>(home);
      home->coherence_home = home->coherence_directory.get();
    }

    auto* dir = home->coherence_directory.get();
    for (auto it = std::begin(path); it != std::prev(std::end(path)); ++it) {
      dir->add_member(ep.cpu, *it);
      (*it)->coherence_home = dir;
    }
  }
}

void resolve(environment& env)
{
  std::vector<CACHE*> caches{};
  for (CACHE& cache : env.cache_view()) {
    caches.push_back(&cache);
  }

  std::vector<entry_point> entry_points{};
  for (O3_CPU& cpu : env.cpu_view()) {
    entry_points.push_back({cpu.cpu, cpu.L1I_bus.lower_level_channel()});
    entry_points.push_back({cpu.cpu, cpu.L1D_bus.lower_level_channel()});
  }

  resolve(caches, entry_points);
}
} // namespace champsim::coherence
