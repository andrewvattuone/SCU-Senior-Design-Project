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

#ifndef COHERENCE_STATE_H
#define COHERENCE_STATE_H

#include <array>
#include <cstdint>
#include <string_view>

namespace champsim
{
/**
 * The MESI state of a cached copy of a block.
 *
 * For a private cache, this is the state of the copy held in that cache.
 *
 * For the cache that acts as the coherence point (see champsim::coherence::directory), this is the directory's view of the copies
 * held *above* it: INVALID if no upper-level cache holds the block, EXCLUSIVE if exactly one core holds a clean copy, MODIFIED if
 * exactly one core holds a copy it may have written, and SHARED if more than one core holds the block.
 */
enum class coherence_state : uint8_t {
  INVALID = 0,
  SHARED,
  EXCLUSIVE,
  MODIFIED,
};

inline constexpr std::array<std::string_view, 4> coherence_state_names{"I", "S", "E", "M"};
} // namespace champsim

#endif
