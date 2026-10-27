# rl_mesi: coherence-aware RL replacement (our improved version)

Our improvement of M. A. Souza and H. C. Freitas, "Reinforcement Learning-Based Cache Replacement Policies for Multicore
Processors," IEEE Access vol. 12, 2024. The faithful reproduction of the paper is a separate module,
[`replacement/souza_freitas`](../souza_freitas/README.md); nothing here changes it.

Select it per cache in a ChampSim config, for example for the private caches as in the paper:

```json
"L1D": { "replacement": "rl_mesi" },
"L2C": { "replacement": "rl_mesi" }
```

## What is kept from the paper

- Inputs: the block's MESI state and whether its sharer count is above half the cores (AT/BT).
- A per-set eviction history of 2 x ways entries; a block that misses while in the history was a bad eviction.
- LRU proposes the victim; the learner can override it.
- The RL-State / RL-Bitset / RL-Both variants (`RL_MODE=state|bitset|both`).

## What is new

### C4: real learning
- **Learned weights.** One weight per feature value ("how good is it to evict a block like this?"), shared by every set of
  the cache. After each outcome, the weights of the features the block had move toward the reward:
  `w = w + alpha * (r - w)`, so every weight stays in [-1, 1]. The paper instead recomputes its weights as the fraction of
  history entries in each state and never applies its reward.
- **Rewards.**

  | Event | Reward |
  |---|---|
  | An evicted block is missed while still in the history | -1 |
  | A history entry ages out without its block returning | +1 |
  | A block that was passed over (protected) is hit | -1 |
  | A protected block is later evicted without having been hit | +1 |
  | A miss caused by another core's invalidation | none |

- **Wider choice.** Among the `RL_WINDOW` least recently used ways, the way with the highest score `V = w[state] + w[sharing]`
  is evicted; ties go to the older way. All weights start at 0, so the policy starts out as LRU. The paper only looks at
  LRU's first and second choices.
- Decisions are applied only when the fill happens: ChampSim can ask for a victim and retry the fill later.

### C5: the I state
- A block that was invalidated by another core and is now being filled again gets the fourth state **Iprev**
  ("previously invalidated"), with its own weight. A resident block is never in I, so plain I would carry no information.
- Detection needs no change to ChampSim: the policy remembers which block it installed in each way. When a fill lands in a way
  that still holds a block the policy never saw evicted, that block was invalidated. A real cache sees the same thing, since
  an invalidated line keeps its tag. Up to `ways` such tags are remembered per set, so a block that returns to a different way
  is still recognized.

### C3: a realistic sharer count
- A private cache only learns how many cores share a block when the directory tells it. The directory now reports the sharer
  count (the number of cores holding the block right after the grant, the requester included) with every grant
  (`channel::response::sharer_count`). The private levels store it per block in `cache_block::delivered_sharers` and pass it
  along: an L2 that fills or answers a hit for its L1 forwards the count it stored, not a fresh one. Merged requests at the
  coherence point are granted in turn, so each core gets the count at its own grant.
- **The count goes stale on purpose.** It is not updated when other cores later read the block (join) or evict it / are
  invalidated (leave); a downgrade from E to S does not refresh it either. It is refreshed to 1 only when this core obtains
  ownership (upgrade from S, or a silent E -> M), since the directory then makes it the sole holder. A store that fills a
  private cache is installed with a count of 1 for the same reason.
- `RL_SHARERS=delivered` (default) uses this stored count in private caches; `RL_SHARERS=oracle` keeps the previous behavior
  of reading the directory's current mask at every decision. At the coherence point both use the directory's own mask, which
  is exact there in hardware too.
- Adding the count changes nothing for other policies: no cache or coherence decision reads it.

### C1: a dynamic sharing threshold (`threshold.h`)
- The paper calls a block "above threshold" when more than half the cores share it, a cut fixed at design time. Here the cut
  `t` (heavy iff count > t) is chosen per cache with `RL_THRESHOLD`:
  - `fixed`: `t = cores / 2`, exactly the paper.
  - `average`: `t` follows a running average (rate 1/16) of the sharer counts of filled blocks, starting at the paper's value.
  - `duel` (default): up to 4 candidate thresholds (2 cores: {0, 1}; 4 cores: {0, 1, 2, 3}; 16 cores: {1, 2, 4, 8}) each lead a
    few sampled sets that always use them. Every 8192 leader accesses the candidate with the lowest miss ratio becomes the
    threshold of all other sets (ties go to the paper's value); counts are halved each epoch so the winner can change with the
    program's phase.
- The learned weight for "heavy" is shared by all sets, so in leader sets it applies to that leader's own cut (the usual
  set-dueling trade-off).

### C2: falling back to BRRIP (`duel.h`)
- Set dueling (DIP, ISCA 2007; DRRIP, ISCA 2010): some leader sets always use this policy, some always use BRRIP, and a 10-bit
  counter (PSEL) moves on every demand fill in a leader set. Follower sets use whichever side is currently missing less. Leader
  selection, BRRIP insertion (distant, every 32nd long) and PSEL width match ChampSim's `replacement/drrip`; there is one PSEL
  per cache, which fits the private L1D/L2.
- LRU order, the RL learning and BRRIP's RRPVs are maintained in every set, so a set can switch sides at once. When BRRIP picks
  the victim, the RL learner still records it in the history and learns from its outcome.
- `RL_DUEL=0` disables the fallback. `RL_MODE=off` always disables it, so that mode stays exactly LRU.

## Run-time switches (environment variables, read once at start)

| Variable | Values (default) | Meaning |
|---|---|---|
| `RL_MODE` | `off`, `state`, `bitset`, `both` (`both`) | Which features are used; `off` is exactly `replacement/lru` |
| `RL_WINDOW` | integer >= 1 (`4`) | How many of the least recently used ways are compared |
| `RL_ALPHA` | 0..1 (`0.1`) | Learning rate |
| `RL_EPSILON` | 0..1 (`0`) | Chance of a random choice inside the window (exploration) |
| `RL_PROTECT` | `0`, `1` (`1`) | Learn from "keep" decisions |
| `RL_ISTATE` | `0`, `1` (`1`) | Use the Iprev state |
| `RL_SHARERS` | `delivered`, `oracle` (`delivered`) | Sharer count in private caches: stored at grant time (C3), or the directory's current mask |
| `RL_THRESHOLD` | `fixed`, `average`, `duel` (`duel`) | How "heavily shared" is decided (C1) |
| `RL_DUEL` | `0`, `1` (`1`) | Set dueling against BRRIP (C2) |

An unrecognized value stops the simulation with an error. `RL_THRESHOLD=fixed RL_DUEL=0` gives the policy without C1 and C2.

## Current limitations (planned next)

- **Full tags and `double` weights.** The storage-cost version (short tags, small integer weights, sampled sets) comes later.
- **One PSEL per cache.** Fine for private caches; a shared last-level cache would want one per core.

## Statistics printed at the end

Per cache: the configuration (including the sharer source), decisions (oldest way vs another way, explorations), the four reward
counts, invalidations seen and Iprev fills, the final weights, and a C3 diagnostic line:

`rl_mesi sharer_count: evaluations: N delivered_above_oracle_below: A delivered_below_oracle_above: B`

In a private cache every feature evaluation computes the threshold test from both sources (whichever one is used). `A` counts
evaluations where the stored count was above threshold but the current one is not (sharers left since the grant), `B` the
reverse (sharers joined). Both are 0 at the coherence point and with one core.

Two more lines describe C1 and C2: `rl_mesi threshold:` (mode, the cut in force, the paper's cut, epochs, the running average),
one `rl_mesi threshold_candidate:` line per candidate (leader sets, epochs won, misses / accesses), and `rl_mesi duel:` /
`rl_mesi duel_misses:` (PSEL, whether followers currently use RL, leader and follower misses, preference flips). The
`decisions` line also counts `brrip_victims`, the evictions chosen by BRRIP.

## Tests

`test/cpp/src/447-rl-mesi.cc` exercises the ChampSim-independent core in `rl_core.h`.
`test/cpp/src/448-mesi-delivered-sharers.cc` checks the count delivered by the directory and its staleness (first reader 1,
second reader 2 while the first keeps 1, writers 1, L2 -> L1 forwarding, merged reads).
`test/cpp/src/449-rl-mesi-duel.cc` covers set dueling and BRRIP; `test/cpp/src/450-rl-mesi-threshold.cc` covers the three
threshold modes.
