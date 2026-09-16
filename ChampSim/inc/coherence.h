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

#ifndef COHERENCE_H
#define COHERENCE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "access_type.h"
#include "address.h"
#include "channel.h"
#include "coherence_state.h"

class CACHE;

namespace champsim
{
struct environment;

namespace coherence
{
struct directory_stats {
  uint64_t grants_exclusive = 0; // read requests satisfied while no other core held the block
  uint64_t grants_shared = 0;    // read requests satisfied while another core held the block
  uint64_t grants_modified = 0;  // read-for-ownership requests
  uint64_t upgrades = 0;         // writes that hit a SHARED copy and had to obtain ownership
  uint64_t invalidations = 0;    // cores whose copies were destroyed by a remote write
  uint64_t downgrades = 0;       // cores whose copies were demoted to SHARED by a remote read
  uint64_t dirty_absorbed = 0;   // requests that destroyed a dirty upper-level copy
  uint64_t releases = 0;         // upper-level evictions that removed a core from a sharer set
  uint64_t dropped_fills = 0;    // fills that were invalidated before they landed
};

/**
 * A precise, zero-latency MESI directory.
 *
 * One directory is owned by each cache that acts as a coherence point (in the default configuration, the LLC). It records, per
 * block, which cores hold a copy above the coherence point and the strongest MESI state among those copies. The coherence point
 * consults the directory whenever it services a request from above; the directory invalidates or downgrades the other cores'
 * copies by calling directly into the affected caches.
 *
 * Modeling simplifications, chosen so that the protocol changes what is cached without adding traffic to the timing model:
 *   - Invalidations, downgrades, upgrades and eviction notifications are instantaneous direct calls, not packets.
 *   - The directory is unbounded and exact. It is not a hardware structure with a size budget.
 *   - When the directory destroys a dirty upper-level copy, the dirtiness is absorbed by the coherence point's copy of the block
 *     (or by the block being filled for the requester), so the eventual writeback to DRAM is preserved.
 *   - The coherence point is not required to be inclusive. Evicting a block from it leaves the upper-level copies and the
 *     directory entry intact.
 */
class directory
{
public:
  struct grant {
    coherence_state state;
    bool absorbed_dirty; // a dirty upper-level copy was destroyed while servicing this request
  };

  explicit directory(CACHE* home);

  void rebind_home(CACHE* home);
  void add_member(uint32_t cpu, CACHE* cache);

  /**
   * Service a request from the given core. Called by the home cache when it supplies a block to an upper level.
   * Reads (LOAD, PREFETCH, TRANSLATION) are granted EXCLUSIVE if no other core holds the block, otherwise SHARED after downgrading
   * the other holders. Read-for-ownership requests (RFO, WRITE) invalidate every other holder and are granted MODIFIED.
   */
  grant request(champsim::address addr, uint32_t cpu, access_type type);

  /**
   * Obtain ownership for a write that hit a non-MODIFIED copy in one of the given core's caches.
   * Every other core's copy is invalidated and every copy held by the requesting core is promoted to MODIFIED.
   * Returns true if a dirty copy was destroyed.
   */
  bool upgrade(champsim::address addr, uint32_t cpu);

  /**
   * Recompute the sharer set for a block after an upper-level copy was evicted or written back.
   */
  void release(champsim::address addr);

  void note_dropped_fill() { ++m_sim_stats.dropped_fills; }

  [[nodiscard]] coherence_state state_of(champsim::address addr) const;
  [[nodiscard]] uint64_t sharers_of(champsim::address addr) const;
  [[nodiscard]] std::vector<uint32_t> cpus_for_channel(const champsim::channel* ch) const;
  [[nodiscard]] std::vector<uint32_t> cpus_for_return_queue(const std::deque<champsim::channel::response_type>* queue) const;
  [[nodiscard]] std::size_t num_tracked() const { return std::size(entries); }
  [[nodiscard]] std::size_t num_cpus() const { return std::size(members); }

  void begin_phase();
  void end_phase();
  [[nodiscard]] const directory_stats& sim_stats() const { return m_sim_stats; }
  [[nodiscard]] const directory_stats& roi_stats() const { return m_roi_stats; }
  void print_stats() const;

  static uint64_t bit(uint32_t cpu) { return uint64_t{1} << cpu; }
  static std::size_t popcount(uint64_t mask);

private:
  struct entry {
    uint64_t sharers = 0;
    coherence_state state = coherence_state::INVALID;
  };

  [[nodiscard]] uint64_t key(champsim::address addr) const;
  [[nodiscard]] bool holds(uint32_t cpu, champsim::address addr) const;
  bool invalidate_cpu(uint32_t cpu, champsim::address addr);
  bool downgrade_cpu(uint32_t cpu, champsim::address addr);
  void refresh_home(champsim::address addr, const entry* e) const;

  CACHE* home;
  std::vector<std::vector<CACHE*>> members{};
  std::unordered_map<uint64_t, entry> entries{};
  directory_stats m_sim_stats{}, m_roi_stats{};
};

struct entry_point {
  uint32_t cpu;
  champsim::channel* channel;
};

/**
 * Discover the coherence domains of a set of caches.
 *
 * Starting from each core's L1 channels, walk downward through lower_level links. The last cache on each path becomes a
 * coherence point and owns a directory; every other cache on the path becomes a member of that directory on behalf of the core.
 * Caches that are not reached (for example, TLBs) are left non-coherent and behave exactly as before.
 */
void resolve(const std::vector<CACHE*>& caches, const std::vector<entry_point>& entry_points);
void resolve(environment& env);

} // namespace coherence
} // namespace champsim

#endif
