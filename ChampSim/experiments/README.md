# Replacement-policy experiments

Sweeps replacement policies (applied to L1D and L2C, as in Souza & Freitas, IEEE Access 2024) over the SPEC
workloads in `workloads.txt`, on the paper's cache hierarchy (32KB L1s, 256KB L2, 16MB 16-way LLC, no prefetchers)
with the MESI directory at the LLC.

## Variants

| variant | cores | address space | trace per core |
|---------|-------|---------------|----------------|
| `p1`    | 1     | -             | the workload |
| `p2`    | 2     | separate      | the same workload on both cores (control: same load, no sharing) |
| `p2s`   | 2     | shared        | the same workload on both cores (creates sharing) |

**Caveat:** the sharing in `p2s` is artificial. Both cores replay the same trace in one address space, so they touch
the same pages in the same order; this exercises the coherence directory but is not a real parallel workload.
Compare `p2s` against `p2` to isolate the effect of sharing.

## Workflow (from a WSL shell in the ChampSim directory)

```bash
bash experiments/build.sh                       # gen_configs.py + one binary per (variant, policy)
bash experiments/run_all.sh                     # every (variant, policy, workload) with a binary and a trace
python3 experiments/parse_results.py            # logs -> results.csv + summary table
```

`build.sh` forwards its arguments to `gen_configs.py`, e.g. `--variants p1 p2s --policies lru srrip souza_freitas`.
`run_all.sh` skips runs whose log is already complete, so it can be re-run to resume.

## Environment variables

| script | variable | default |
|--------|----------|---------|
| build.sh | `CHAMPSIM_BUILD_DIR` | `~/champsim-build` |
| build.sh | `MAKE_JOBS` | `nproc` |
| run_all.sh | `BUILD_DIR` | `~/champsim-build` |
| run_all.sh | `TRACE_DIR` | `~/traces` |
| run_all.sh | `OUT` | `~/champsim-results` |
| run_all.sh | `WARMUP` / `SIM` | `10000000` / `50000000` |
| run_all.sh | `JOBS` | `8` (size this to WSL's cores and memory) |
| run_all.sh | `VARIANTS` / `POLICIES` / `WORKLOADS` | all (space-separated filters) |

## Outputs

- `$OUT/raw/<variant>/<policy>/<workload>.log`: raw ChampSim output (`.log.tmp` while running or after a failure).
- `$OUT/results.csv`: one row per (variant, policy, workload, cpu) with IPC, demand (LOAD+RFO) accesses, misses,
  hit rate and MPKI plus total accesses/misses for L1D, L2C and LLC, the LLC coherence counters (blank when the
  run has no directory), and wall time. `parse_results.py <results dir> --csv <file>` overrides the paths.
