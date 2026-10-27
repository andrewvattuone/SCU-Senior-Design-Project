# Replacement-policy experiments

Sweeps replacement policies (applied to L1D and L2C, as in Souza & Freitas, IEEE Access 2024) over the SPEC
and CloudSuite workloads in `workloads.txt`, on the paper's cache hierarchy (32KB L1s, 256KB L2, 16MB 16-way LLC, no prefetchers)
with the MESI directory at the LLC.

## Variants

| variant | cores | address space | trace per core |
|---------|-------|---------------|----------------|
| `p1`    | 1     | -             | the workload |
| `p2`    | 2     | separate      | the same workload on both cores (control: same load, no sharing) |
| `p2s`   | 2     | shared        | the same workload on both cores (creates sharing) |
| `p4`    | 4     | separate      | per-core workload: core i runs its own trace; otherwise the same trace on every core |
| `p4s`   | 4     | shared        | as `p4`, in one address space (per-core CloudSuite threads -> real sharing) |

`build.sh`/`gen_configs.py` build `p1 p2 p2s` by default; pass `--variants p4 p4s` for the 4-core variants.
`run_all.sh` likewise defaults to `VARIANTS="p1 p2 p2s"`.

**Caveat:** the sharing in `p2s` is artificial. Both cores replay the same trace in one address space, so they touch
the same pages in the same order; this exercises the coherence directory but is not a real parallel workload.
Compare `p2s` against `p2` to isolate the effect of sharing.

## Workload file

`workloads.txt` columns: `name trace behavior [format]`.

- `trace` is relative to `TRACE_DIR`. `.champsimtrace.xz` is appended unless it already ends in `.xz`.
- A `{core}` placeholder makes the workload per-core: core i of an N-core variant runs the trace with `{core}`
  replaced by i (i = 0..N-1). Without it (SPEC) every core runs the same trace.
- `format` is `champsim` (default) or `cloudsuite` (runs ChampSim with `--cloudsuite`).

The CloudSuite entries (behavior `server`) use the CRC-2 per-core traces listed in
https://raw.githubusercontent.com/CMU-SAFARI/Pythia/master/scripts/artifact_traces.csv, stored as
`$TRACE_DIR/cloudsuite/<app>_phase<k>_core<i>.trace.xz` (about 100M instructions each). That list only has complete
4-core sets for cassandra (phases 0, 1, 5), nutch (0, 1, 3, 4) and streaming (2); classification and cloud9 are
not in it. Run them on `p4`/`p4s`:

```bash
bash experiments/build.sh --variants p4 p4s --policies lru srrip drrip souza_freitas rl_mesi
VARIANTS="p4 p4s" WORKLOADS="cassandra nutch streaming" WARMUP=5000000 SIM=20000000 bash experiments/run_all.sh
```

**CloudSuite caveats:** the per-core traces are threads of one server process, so `p4s` gives real (mostly
read-only) sharing. `shared_address_space` maps every core to ASID 0 and ChampSim ignores the per-instruction
ASIDs in the trace; nearly all memory references use one data context, so this changes little, but `p4` (separate
address spaces) turns the threads into four unrelated processes. The traces are SPARC (Solaris), so many user
addresses are in the `0xffff...` range; on Solaris/SPARC64 that is where user stacks and shared libraries live (the
kernel runs in its own context), so it is most likely not kernel data.

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
