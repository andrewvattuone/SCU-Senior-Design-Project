#!/usr/bin/env python3
"""
Generate one ChampSim configuration per (variant, policy) pair into experiments/configs/.

The cache hierarchy follows Souza & Freitas, "Reinforcement Learning-Based Cache Replacement Policies for Multicore
Processors" (IEEE Access 2024), Sec. III-B: private 32KB 8-way L1I/L1D, private 256KB 8-way L2, a 16MB 16-way LLC,
64B lines, and no prefetchers. The policy under test replaces both L1D and L2 at once, as in the paper; L1I and the
LLC stay on LRU.

Variants:
  p1   1 core
  p2   2 cores, separate address spaces (no cross-core sharing)
  p2s  2 cores, shared address space (run the same trace on both cores to create sharing)
"""

import argparse
import copy
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
BASE_CONFIG = HERE.parent / 'champsim_2core_shared_config.json'

PAPER_CACHES = {
    'L1I': {'sets': 64, 'ways': 8},
    'L1D': {'sets': 64, 'ways': 8},
    'L2C': {'sets': 512, 'ways': 8},
    'LLC': {'sets': 16384, 'ways': 16},
}

VARIANTS = {
    'p1': {'num_cores': 1, 'shared_address_space': False},
    'p2': {'num_cores': 2, 'shared_address_space': False},
    'p2s': {'num_cores': 2, 'shared_address_space': True},
}

DEFAULT_POLICIES = ['lru', 'srrip', 'drrip']


def make_config(base: dict, variant: str, policy: str) -> dict:
    cfg = copy.deepcopy(base)
    cfg['executable_name'] = f'champsim_{variant}_{policy}'
    cfg['num_cores'] = VARIANTS[variant]['num_cores']
    cfg['virtual_memory'] = {'shared_address_space': VARIANTS[variant]['shared_address_space']}

    for name, geometry in PAPER_CACHES.items():
        cfg[name].update(geometry)
        cfg[name]['prefetcher'] = 'no'

    cfg['L1I']['replacement'] = 'lru'
    cfg['L1D']['replacement'] = policy
    cfg['L2C']['replacement'] = policy
    cfg['LLC']['replacement'] = 'lru'
    return cfg


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--variants', nargs='+', default=list(VARIANTS), choices=list(VARIANTS))
    parser.add_argument('--policies', nargs='+', default=DEFAULT_POLICIES,
                        help='replacement module names under replacement/ (default: %(default)s)')
    parser.add_argument('--out', type=pathlib.Path, default=HERE / 'configs')
    args = parser.parse_args()

    # The hand-written base config repeats some keys; json keeps the last one, which is what ChampSim's parser does too.
    base = json.loads(BASE_CONFIG.read_text())

    args.out.mkdir(parents=True, exist_ok=True)
    for stale in args.out.glob('*.json'):
        stale.unlink()

    for variant in args.variants:
        for policy in args.policies:
            path = args.out / f'{variant}_{policy}.json'
            path.write_text(json.dumps(make_config(base, variant, policy), indent=2) + '\n', newline='\n')
            print(f'wrote {path.relative_to(HERE.parent)}')


if __name__ == '__main__':
    main()
