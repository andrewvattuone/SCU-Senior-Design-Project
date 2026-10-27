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

#ifndef BLOCK_H
#define BLOCK_H

#include "champsim.h"
#include "coherence_state.h"

namespace champsim
{
struct cache_block {
  bool valid = false;
  bool prefetch = false;
  bool dirty = false;

  champsim::address address{};
  champsim::address v_address{};
  champsim::address data{};

  uint32_t pf_metadata = 0;

  // MESI state of this copy. At a coherence point, this is the directory's view of the copies held above it.
  coherence_state state = coherence_state::INVALID;
  // At a coherence point, one bit per core that holds a copy above it. Zero elsewhere.
  uint64_t sharers = 0;
  // Private (non-coherence-point) caches only: the sharer count reported by the directory when this copy was granted, as a real
  // private cache would learn it from the grant message. It MAY BE STALE: it is not updated when other cores later join (read the
  // block) or leave (evict it, or are invalidated). It is refreshed only when this core obtains ownership through an upgrade or a
  // silent E -> M transition, which make this core the sole holder (count 1). Always >= 1 for a valid copy in a coherent private
  // cache; 0 at the coherence point (use `sharers` there) and in caches outside any coherence domain.
  uint32_t delivered_sharers = 0;
};
} // namespace champsim

#endif
