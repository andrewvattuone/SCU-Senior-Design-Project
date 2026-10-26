# souza_freitas — RL-based coherence-aware replacement

Reproduction of M. A. Souza and H. C. Freitas, "Reinforcement Learning-Based Cache Replacement Policies for
Multicore Processors," *IEEE Access*, vol. 12, 2024 (RL-State, RL-Bitset, and a combined variant on top of LRU or SRRIP).

## Files

| File | Role |
|------|------|
| `sf_core.h` | Header-only, ChampSim-independent core: eviction history, weights, decision rule, LRU/SRRIP helpers, stats. |
| `souza_freitas.h/.cc` | Thin ChampSim wrapper wiring the four replacement hooks to the core. |
| `test/cpp/src/446-souza-freitas.cc` | Catch2 unit tests for the core. |

## Selecting it

In a config JSON, per cache (the paper applies it to the private L1 and L2):

```json
"L1D": { "replacement": "souza_freitas" },
"L2C": { "replacement": "souza_freitas" }
```

Variants are chosen at run time (read once when each cache is constructed), so no rebuild is needed:

| Variable | Values | Default |
|----------|--------|---------|
| `SF_MODE` | `off` (base policy unchanged), `state` (RL-State), `bitset` (RL-Bitset), `both` | `state` |
| `SF_BASE` | `lru`, `srrip` | `lru` |
| `SF_RULE` | `text`, `example` (see below) | `text` |

An unrecognized value aborts construction with `std::invalid_argument`. Example:
`SF_MODE=bitset SF_BASE=srrip bin/champsim ...`

`replacement_final_stats` prints the active configuration, decisions, first-candidate accepts, second chances,
empty-history decisions, rewards (-1 history hits, +1 otherwise), refill removals, history evictions by age, the average
weights at decision time, and the final weights averaged over sets with a non-empty history.

## What it does

- **Eviction History Table** (one per set, capacity 2 x associativity): each entry holds the block tag, an above/below
  threshold bit, and the 2-bit MESI state at eviction. Evicting a block adds its entry; when full, the least recently
  updated entry is dropped. When a block is filled, any history entry with its tag is removed.
- **Threshold**: a block is above threshold (AT) iff its sharer count is greater than `NUM_CPUS / 2`.
- **Weights**: recomputed from table contents. `wM, wE, wS` = fraction of entries in that state; `wAT, wBT` = fraction
  above/below threshold. There is no learning-rate update (none in the paper).
- **Rewards**: on every access, if the tag is in the history the reward is -1 and the entry is removed, otherwise +1.
  Rewards only influence the policy through entry removal; they are counted as statistics.
- **Decision**: the base policy picks candidate #1. If the RL check accepts it, it is evicted. Otherwise it gets its single
  second chance and the base policy's second choice is evicted unconditionally.

## Interpretation choices

1. **Decision rule contradiction (`SF_RULE`).** The §II-C text says evict if the block corresponds to a high-probability
   element; the §II-C example keeps an M block *because* wM is highest. `text`: evict iff the candidate's state/class has
   the highest weight. `example`: keep iff it has the highest weight. Ties count as highest in both.
2. **Empty history.** With no distribution yet, the initial bias applies regardless of `SF_RULE`: the state check evicts
   only Exclusive blocks, the bitset check only below-threshold blocks. State weights are computed over entries whose
   state is known (not I); if there are none, the state check uses the bias.
3. **RL-Both = AND.** The paper does not define the combination precisely; `both` evicts candidate #1 only if both the
   state check and the bitset check say evict.
4. **Oracle sharer information.** The paper assumes private caches know a block's sharer count. Private caches here
   hold `sharers == 0`, so the module reads the true mask from the MESI directory (`coherence_home->sharers_of`) at
   decision time. At a coherence point the block's own `sharers` field is used. Caches outside a coherence domain see 0.
5. **INVALID state.** A candidate whose state is I (non-coherent cache, no information) is accepted as candidate #1.
6. **Next attempt.** Only one second chance. LRU: the second least recently used way. SRRIP: the first choice is made
   exactly as `replacement/srrip` does (aging all ways); if rejected, the candidate's RRPV is lowered to `maxRRPV - 1`
   and the search repeats over the other ways, aging only them, so the protected block cannot be chosen.
7. **Writebacks.** Writebacks from the upper level (`access_type::WRITE`) produce no reward and no history lookup, and
   (as in `replacement/lru`) do not update LRU recency on a hit. Their fills still evict and record a victim, and still
   remove a matching history entry.
8. **History tag** is the block address (`full_addr >> OFFSET_BITS`) as seen by the replacement hooks, used
   consistently for insertion (`victim_addr`) and lookup. The victim's state and sharer class are captured in
   `find_victim`, since `replacement_cache_fill` only receives its address.
9. **`SF_MODE=off`** reproduces `lru` / `srrip` exactly (same recency/RRPV updates and tie-breaking) and skips all
   history bookkeeping.

Only LRU and SRRIP bases are implemented; the paper's LFU, MRU, NRU and PLRU baselines are not.
