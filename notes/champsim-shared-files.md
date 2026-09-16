# ChampSim — Shared Reading Notes

Notes on the four files everyone on the team reviews.

Assumes the default config in `champsim_config.json` (1 core @ 4 GHz, 64-byte lines,
L1D 64 sets × 12 ways, L2C 1024 × 8, LLC 2048 × 16).

---

## How the four files fit together

```
        CPU core
           |
      [ channel ]      <-- inc/channel.h : the pipe between levels
           |
        L1D CACHE      <-- inc/cache.h  : what a cache IS  (state + knobs)
           |              src/cache.cc  : what a cache DOES (per-cycle behavior)
      [ channel ]
           |
        L2C CACHE  ---> asks "who do I evict?" ---> replacement/lru/*
           |
      [ channel ]
           |
        LLC CACHE
           |
         DRAM
```

Key idea: **there is only one `CACHE` class.** L1I, L1D, L2C, LLC and all three TLBs
are the same C++ class with different numbers plugged in at build time. There is no
`class LLC` anywhere. Levels are distinguished only by their config values and by
which channels they are wired to.

---

## inc/channel.h — the pipe between levels

Only 117 lines. Read this first; everything else assumes it.

A `channel` is the connection between one cache and the level below it. It is a
dumb mailbox — it holds packets, it does not process them.

### What's inside

| Member | What it is |
|---|---|
| `RQ` | Read Queue — normal loads coming down |
| `WQ` | Write Queue — writebacks coming down |
| `PQ` | Prefetch Queue — prefetch requests coming down |
| `returned` | responses coming back **up** |

Three queues down, one queue up. Separating RQ/WQ/PQ lets the cache prioritize
(a real load should not be stuck behind a pile of prefetches).

### The two packet types

**`request`** — going down. Fields worth knowing:

- `address` / `v_address` — physical and virtual
- `is_translated` — **defaults to `true`**, but the core sets it `false`.
  This is what triggers the TLB detour.
- `type` — LOAD / WRITE / PREFETCH / etc.
- `instr_depend_on_me` — list of instruction IDs waiting on this
- `pf_metadata` — a `uint32_t` scratchpad that rides along with the request and
  comes back on the response. This is how a prefetcher passes information from
  "I issued this" to "this landed."
- `response_requested` — false for writebacks (fire and forget, nobody waits)

**`response`** — coming back up. Smaller: address, data, `pf_metadata`,
and the `instr_depend_on_me` list so the core knows who to wake.

### The important behavior: backpressure

`add_rq` / `add_wq` / `add_pq` return **`bool`**. They return `false` if the queue
is full, and the caller must retry later.

This is the whole flow-control mechanism of the simulator. There is no global
scheduler. A full queue simply refuses work, which stalls the level above, which
stalls the level above that. Backpressure propagates naturally.

> **Gotcha:** if you ever add code that calls `add_*` and ignores the return value,
> you silently drop requests. That shows up much later as a deadlock abort.

`cache_queue_stats` counts `RQ_ACCESS` / `RQ_FULL` / `RQ_TO_CACHE` per queue —
useful for telling "my prefetcher is bad" apart from "my prefetcher is fine but
the PQ is always full."

---

## inc/cache.h — what a cache *is*

538 lines. This is the declaration: state, config, and the module interfaces.

### Config knobs (map 1:1 onto the JSON)

`cache.h:157-169`

| Member | JSON key |
|---|---|
| `NUM_SET`, `NUM_WAY` | `sets`, `ways` |
| `HIT_LATENCY` | `latency` |
| `FILL_LATENCY` | (derived) |
| `MSHR_SIZE` | `mshr_size` |
| `MAX_TAG`, `MAX_FILL` | `max_tag_check`, `max_fill` |
| `NAME` | `"L1D"`, `"LLC"`, … |

These are set once at construction by the generated `core_inst.inc`.
**Changing a cache size means re-running `./config.sh`, not just `make`.**

### Storage

```cpp
set_type block{NUM_SET * NUM_WAY};   // ONE flat std::vector
```

Not a 2D array. `get_set_index()` (`cache.cc:527`) slices bits out of the address —
skip the low `OFFSET_BITS`, take `lg2(NUM_SET)` bits — and `get_set_span()` returns
the iterator pair for that slice. A lookup is a linear scan of ~12 ways.

### Two packet types (internal, not the channel ones)

- **`tag_lookup_type`** — a request in flight *inside* this cache. Carries
  `event_cycle` (when the tag check may run) and `to_return` (who to notify).
- **`fill_type`** — data on its way back in. Holds a
  `champsim::waitable<returned_value> data_promise` — a value that is not
  readable until a given time. **That is how fill latency is modeled.**
  Has a static `merge()` — the basis of miss merging.

### Queues held by the cache itself

| Queue | Holds |
|---|---|
| `inflight_tag_check` | requests waiting out `HIT_LATENCY` |
| `translation_stash` | requests parked waiting on the TLB |
| `MSHR` | misses sent down, awaiting a response |
| `inflight_fills` | responses back, waiting out `FILL_LATENCY` |
| `internal_PQ` | prefetches this cache generated itself |

`translation_stash` matters: an untranslated request is **moved aside**, not left
blocking. Other requests flow past it.

### Where our code plugs in

`cache.h:224-296`

```cpp
std::unique_ptr<prefetcher_module_concept>  pref_module_pimpl;
std::unique_ptr<replacement_module_concept> repl_module_pimpl;
```

Two abstract interfaces. `CACHE` calls through the pointer and never knows what is
on the other end. That is why `"prefetcher": "no"` → `"prefetcher": "mine"` needs
zero changes to `cache.cc`.

**Prefetcher hooks:** `cache_operate` (every access, hit or miss),
`cache_fill` (a line landed; tells you what got evicted), `cycle_operate`
(every cycle), `branch_operate` (L1I only), `initialize`, `final_stats`.

**Replacement hooks:** `find_victim`, `update_replacement_state`,
`replacement_cache_fill`, `initialize_replacement`, `final_stats`.

Call back into the cache with:

```cpp
prefetch_line(pf_addr, fill_this_level, prefetch_metadata);
```

---

## src/cache.cc — what a cache *does*

927 lines. `CACHE::operate()` at **line 417** is the one function to understand.

### operate() runs backwards (finish old work before starting new)

1. **Finish returns** (`:429`) — drain `lower_level->returned`, hand each to
   `finish_packet`, which finds the matching MSHR entry and moves it to `inflight_fills`.
2. **Finish translations** (`:434`) — same, for TLB results.
3. **Perform fills** (`:441`) — for fills whose `data_promise.is_ready_at(now)`,
   call `handle_fill`.
4. **Initiate tag checks** (`:448`) — pull new requests from upper levels'
   WQ/RQ/PQ into `inflight_tag_check`.
5. **Issue translations** (`:486`) — send untranslated entries to the TLB; park
   them in `translation_stash` if not ready.
6. **Perform tag checks** (`:496`) — for entries whose `event_cycle` has arrived,
   run `try_hit`, then `handle_miss` / `handle_write` for the rest.
7. **`impl_prefetcher_cycle_operate()`** (`:511`) — prefetcher gets a tick regardless.

### Two modeling idioms used everywhere

**Latency is a timestamp, not a sleep.** Nothing blocks. A request gets
`event_cycle = now + HIT_LATENCY` and step 6 simply ignores it until the clock
catches up. A 20-cycle LLC just means entries sit in `inflight_tag_check` longer.

**Bandwidth is a budget object.** `champsim::bandwidth tag_check_bw{MAX_TAG}` is
created fresh each cycle and `consume()`d. When it runs out, the rest wait for
next cycle. That is how `"max_tag_check": 1` models a single-ported cache.

### The four functions that do the real work

**`try_hit` (`:247`)** — `std::find_if` over the ways for a valid matching block.
Then, in order: call the prefetcher (`impl_prefetcher_cache_operate`), call
`impl_update_replacement_state`, and if hit, push a response to everyone in
`to_return`, set the dirty bit on a write, and if this was a *useful prefetch*
(`hit && way->prefetch && !prefetch_from_this`) bump `pf_useful` and clear the
prefetch bit.

> **Note for replacement work:** `update_replacement_state` is called on **both
> hits and misses** — the `hit` flag tells you which. Only updating on hits is a
> classic bug.

**`handle_miss` (`:317`)** — this is where **miss merging** happens.
Before allocating, it searches `MSHR` *and* `inflight_fills` for the same address
(`:332-338`). If found, the new request attaches to the existing entry instead of
issuing a second request downstream. If the existing entry was a PREFETCH and the
new one is a real demand access, it counts a `pf_useful`.

> **This is why prefetch timing is forgiving.** A prefetch still in flight when the
> real load arrives is not wasted — the load merges onto it. The prefetch only has
> to *start* early, not *finish* early.

**`handle_fill` (`:169`)** — the eviction path:

1. Look for an invalid (empty) way first (`:175`).
2. Only if the set is full, call `impl_find_victim` (`:177`). **Our replacement
   policy is not consulted on a cold set.**
3. If the victim is `valid && dirty`, build a writeback packet and
   `lower_level->add_wq(...)`. **If that fails, `handle_fill` returns `false` and
   the whole fill is retried next cycle** (`:207-210`).
4. Call `impl_prefetcher_cache_fill`, then `impl_replacement_cache_fill`.
5. If the evicted block was a prefetch that was never used, bump `pf_useless`.

**`handle_write` (`:376`)** — writebacks skip the tag check path and go straight
into `inflight_fills` with `FILL_LATENCY`. Counted as a miss.

**`finish_packet` (`:606`)** — note `assert(0)` if no MSHR matches an incoming
response. If you ever see "cannot find a matching entry", something issued a
request without an MSHR.

### print_deadlock

Override that makes the Stage-1 abort useful — dumps every queue and MSHR with
instruction IDs and addresses. Read it bottom-up: whichever level has an entry
with nothing below it working on that address is the bug.

---

## replacement/lru/lru.{h,cc} — the template for our policy

36 lines of `.cc`. This is the shape our own policy will take.

### State

```cpp
long NUM_WAY;
std::vector<uint64_t> last_used_cycles;   // one counter per (set, way)
uint64_t cycle = 0;                       // NOT the simulator clock
```

`last_used_cycles` is flat, indexed `set * NUM_WAY + way` — same flattening trick
as the block array.

`cycle` is a **private monotonic counter, not the real clock.** It just increments
on every touch. LRU only needs relative ordering, so this is cheaper and cannot
drift with the simulator's time handling. Worth copying.

### find_victim

```cpp
auto victim = std::min_element(begin, end);
return std::distance(begin, victim);
```

Smallest counter = touched longest ago = evict it. Returns a **way index**, not a
pointer. Three lines.

### update_replacement_state

```cpp
if (hit && access_type{type} != access_type::WRITE)
    last_used_cycles.at(set * NUM_WAY + way) = cycle++;
```

Two things to notice:

- **Only updates on a hit.** On a miss there is no meaningful way to update yet —
  `replacement_cache_fill` handles that when the line actually lands.
- **Skips writeback hits.** A writeback arriving from above is not the program
  using the data; counting it would corrupt the recency order.

### replacement_cache_fill

Unconditionally stamps the newly-filled way with the current counter, so a fresh
line starts as most-recently-used.

### Constructor pattern

```cpp
lru::lru(CACHE* cache) : lru(cache, cache->NUM_SET, cache->NUM_WAY) {}
```

Delegating constructor — the one-arg version reads sizes off the cache and forwards.
Our policy should follow the same pattern so it works at any level unchanged.

---

## Cross-cutting things to remember

1. **Nothing is ever computed.** No instruction is executed, no data value is real.
   The simulator models *how long things take and what waits on what*. `data` fields
   exist but are not meaningful values.

2. **Warmup shortcuts almost everything.** `FILL_LATENCY` and `HIT_LATENCY` become
   zero, DRAM answers instantly, register dependencies are erased. Warmup fills
   caches/predictors fast; it does not model timing.

3. **Physical addresses are synthetic and randomized** (`"randomization": 1`).
   Only the virtual addresses from the trace carry meaning.

4. **Every `operate()` returns a progress count.** Return 0 from everything for 500
   straight cycles and the simulator dumps state and aborts. If we add a module that
   does work but reports no progress, we can trip this falsely.

5. **Hit rate is not the score. IPC is.** The core has a 352-entry ROB and can hide
   a lot of misses on its own. Eliminating a miss the core was already hiding shows
   up in coverage and not in IPC.

---

## Open questions for the team

- [ ] Which level are we targeting — L1D, L2C, or LLC? Changes the space/latency
      tradeoff completely.
- [ ] Are we doing a prefetcher, a replacement policy, or both?
- [ ] What are we using `pf_metadata` for? It is our only free channel from issue
      to fill, and it is only 32 bits.
- [ ] What is our baseline? (`"prefetcher": "no"`, `"replacement": "lru"`) — we need
      a committed set of numbers before we change anything.
- [ ] Which traces, and how many instructions? Warmup defaults to 20% of the
      simulation count if we do not set it explicitly.

---

## inc/coherence.h + src/coherence.cc — MESI directory (added Sep 2026)

ChampSim upstream has no coherence. We added a MESI directory so that `state` and `sharers`
exist on every `cache_block`. Full write-up: `ChampSim/docs/src/Coherence.rst`.

### The one-paragraph version

The last cache before DRAM (the LLC) is the **coherence point** and owns a
`champsim::coherence::directory`. Every request that reaches the LLC from above — hit or
miss — is granted a state: a read gets **E** if nobody else holds the block, otherwise
everyone is downgraded and the reader gets **S**; a store miss (RFO) invalidates every other
copy and gets **M**. A store that *hits* an S line asks the directory for an **upgrade**.
Private-cache evictions notify the directory so the sharer set stays exact. The topology is
discovered automatically at startup (`coherence::resolve(env)`), so no config keys.

### What a replacement policy sees

| Field on `champsim::cache_block` | In a private cache (L1D, L2C) | In the LLC (coherence point) |
|---|---|---|
| `state` | MESI state of *this* copy | Directory view of the copies above: I = no core holds it, E/M = one core, S = several |
| `sharers` | always 0 | bitmask, one bit per core holding the block |

Both are readable from `find_victim()` via `current_set[way]`, and from any hook via
`intern_->block[set * NUM_WAY + way]`. This is exactly the `mesi_state` / `sharer_count`
pair our RL policy learns over.

### Gotchas

- **Multi-programmed traces never share.** ChampSim maps pages per `(core, vpage)`, so two
  cores never touch the same physical block and invalidations/downgrades are zero. To get
  real sharing, set `"virtual_memory": { "shared_address_space": true }` (we added this knob)
  and run the *same* trace on every core — see `champsim_2core_shared_config.json`.
- Single-core is bit-identical to upstream (verified: same IPC/cycles/LLC counts). Coherence
  only changes behavior when a second core exists.
- **Build only with `bash wsl-build.sh`** from WSL. The native Makefile breaks on the space in
  `Senior Design` and the CRLF checkout. The script mirrors to `~/champsim-build` and builds
  there; traces live in WSL at `~/traces`.
- Directory actions are zero-latency direct calls (no packets, no queue pressure). The
  directory is exact and unbounded — it is a modeling device, not a hardware budget.
- Tests: `test/cpp/src/440-coherence-mesi-states.cc` walks the full E → S → M → invalidate →
  downgrade → evict sequence on a two-core mock hierarchy.
