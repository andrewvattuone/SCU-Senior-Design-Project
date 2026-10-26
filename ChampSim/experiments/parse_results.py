#!/usr/bin/env python3
"""
Collect the logs written by run_all.sh into one CSV row per (variant, policy, workload, cpu).

Logs are read from <results>/raw/<variant>/<policy>/<workload>.log. Per-CPU statistics come only from the region
after "=== Simulation ==="; the coherence directory counters are printed once per run, after that region.
"""

import argparse
import collections
import csv
import os
import pathlib
import re
import statistics
import sys

HERE = pathlib.Path(__file__).resolve().parent

LEVELS = ['L1D', 'L2C', 'LLC']
DEMAND_TYPES = ('LOAD', 'RFO')

IPC_RE = re.compile(r'^CPU (\d+) cumulative IPC: ([\d.]+) instructions: (\d+) cycles: (\d+)')
CACHE_RE = re.compile(r'^cpu(\d+)->(\S+) (TOTAL|LOAD|RFO|PREFETCH|WRITE|TRANSLATION)\s+'
                      r'ACCESS:\s+(\d+) HIT:\s+(\d+) MISS:\s+(\d+)')
HEADER_RE = re.compile(r'^(Warmup|Simulation) Instructions: (\d+)')
TIME_RE = re.compile(r'\(Simulation time: (\d+) hr (\d+) min (\d+) sec\)')
COHERENCE_RES = [
    re.compile(r'COHERENCE GRANTS\s+E: (?P<grants_e>\d+)\s+S: (?P<grants_s>\d+)\s+M: (?P<grants_m>\d+)\s+'
               r'UPGRADES: (?P<upgrades>\d+)'),
    re.compile(r'COHERENCE INVALIDATIONS: (?P<invalidations>\d+)\s+DOWNGRADES: (?P<downgrades>\d+)\s+'
               r'DIRTY ABSORBED: (?P<dirty_absorbed>\d+)\s+RELEASES: (?P<releases>\d+)\s+'
               r'DROPPED FILLS: (?P<dropped_fills>\d+)'),
    re.compile(r'COHERENCE DROPPED WRITEBACKS: (?P<dropped_writebacks>\d+)'),
]
COHERENCE_FIELDS = [name for regex in COHERENCE_RES for name in regex.groupindex]

COLUMNS = (['variant', 'policy', 'workload', 'behavior', 'cpu', 'warmup', 'sim', 'instructions', 'cycles', 'ipc']
           + [f'{lvl}_{stat}' for lvl in LEVELS
              for stat in ('demand_access', 'demand_miss', 'demand_hit_rate', 'demand_mpki',
                           'total_access', 'total_miss')]
           + [f'coh_{name}' for name in COHERENCE_FIELDS]
           + ['sim_seconds'])


def load_behaviors(path: pathlib.Path) -> dict[str, str]:
    behaviors = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 3 and not fields[0].startswith('#'):
            behaviors[fields[0]] = fields[2]
    return behaviors


def cache_name(raw: str) -> str:
    return re.sub(r'^cpu\d+_', '', raw)


def parse_log(text: str) -> tuple[dict, dict[int, dict]]:
    """Return (run-wide fields, per-cpu fields) for one log."""
    run: dict = {'warmup': '', 'sim': '', 'sim_seconds': ''}
    run.update({f'coh_{name}': '' for name in COHERENCE_FIELDS})
    cpus: dict[int, dict] = collections.defaultdict(lambda: {'caches': collections.defaultdict(dict)})

    lines = text.splitlines()
    in_sim = False
    for line in lines:
        if line.startswith('=== Simulation ==='):
            in_sim = True
            continue
        header = HEADER_RE.match(line)
        if header:
            run['warmup' if header.group(1) == 'Warmup' else 'sim'] = int(header.group(2))
        time = TIME_RE.search(line)
        if time:
            hours, minutes, seconds = map(int, time.groups())
            run['sim_seconds'] = hours * 3600 + minutes * 60 + seconds
        for regex in COHERENCE_RES:
            match = regex.search(line)
            if match:
                run.update({f'coh_{k}': int(v) for k, v in match.groupdict().items()})
        if not in_sim:
            continue
        ipc = IPC_RE.match(line)
        if ipc:
            cpu = cpus[int(ipc.group(1))]
            cpu['ipc'] = float(ipc.group(2))
            cpu['instructions'] = int(ipc.group(3))
            cpu['cycles'] = int(ipc.group(4))
            continue
        cache = CACHE_RE.match(line)
        if cache:
            cpu_id, raw_name, kind, access, _hit, miss = cache.groups()
            cpus[int(cpu_id)]['caches'][cache_name(raw_name)][kind] = (int(access), int(miss))
    return run, dict(cpus)


def level_fields(level: str, caches: dict, instructions: int | None) -> dict:
    stats = caches.get(level)
    if not stats:
        return {}
    access = sum(stats.get(kind, (0, 0))[0] for kind in DEMAND_TYPES)
    miss = sum(stats.get(kind, (0, 0))[1] for kind in DEMAND_TYPES)
    total_access, total_miss = stats.get('TOTAL', ('', ''))
    return {
        f'{level}_demand_access': access,
        f'{level}_demand_miss': miss,
        f'{level}_demand_hit_rate': round(1 - miss / access, 6) if access else '',
        f'{level}_demand_mpki': round(miss * 1000 / instructions, 4) if instructions else '',
        f'{level}_total_access': total_access,
        f'{level}_total_miss': total_miss,
    }


def collect(results: pathlib.Path, behaviors: dict[str, str]) -> list[dict]:
    rows = []
    for log in sorted(results.glob('raw/*/*/*.log')):
        variant, policy, workload = log.parent.parent.name, log.parent.name, log.stem
        run, cpus = parse_log(log.read_text(errors='replace'))
        if not cpus:
            print(f'warning: no simulation statistics in {log}', file=sys.stderr)
            continue
        for cpu_id, cpu in sorted(cpus.items()):
            row = {'variant': variant, 'policy': policy, 'workload': workload,
                   'behavior': behaviors.get(workload, ''), 'cpu': cpu_id,
                   'instructions': cpu.get('instructions', ''), 'cycles': cpu.get('cycles', ''),
                   'ipc': cpu.get('ipc', '')}
            row.update(run)
            for level in LEVELS:
                row.update(level_fields(level, cpu['caches'], cpu.get('instructions')))
            rows.append(row)
    return rows


def print_summary(rows: list[dict]) -> None:
    groups: dict[tuple[str, str], list[dict]] = collections.defaultdict(list)
    for row in rows:
        groups[(row['variant'], row['policy'])].append(row)

    def mean(group: list[dict], column: str) -> str:
        values = [r[column] for r in group if r.get(column) not in ('', None)]
        return f'{statistics.fmean(values):.3f}' if values else '-'

    print(f'{"variant":<8} {"policy":<16} {"rows":>5} {"ipc":>8} {"L1D_mpki":>10} {"L2C_mpki":>10}')
    for (variant, policy), group in sorted(groups.items()):
        print(f'{variant:<8} {policy:<16} {len(group):>5} {mean(group, "ipc"):>8} '
              f'{mean(group, "L1D_demand_mpki"):>10} {mean(group, "L2C_demand_mpki"):>10}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('results', nargs='?', type=pathlib.Path,
                        default=pathlib.Path(os.environ.get('OUT', '~/champsim-results')).expanduser(),
                        help='results directory containing raw/ (default: $OUT or ~/champsim-results)')
    parser.add_argument('--csv', type=pathlib.Path, help='output CSV (default: <results>/results.csv)')
    parser.add_argument('--workloads', type=pathlib.Path, default=HERE / 'workloads.txt')
    args = parser.parse_args()

    rows = collect(args.results, load_behaviors(args.workloads))
    if not rows:
        sys.exit(f'no parsable logs under {args.results / "raw"}')

    out = args.csv or args.results / 'results.csv'
    with out.open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=COLUMNS, restval='')
        writer.writeheader()
        writer.writerows(rows)
    print(f'wrote {len(rows)} rows to {out}\n')
    print_summary(rows)


if __name__ == '__main__':
    main()
