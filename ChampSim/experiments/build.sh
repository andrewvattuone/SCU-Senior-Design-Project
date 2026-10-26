#!/usr/bin/env bash
#
# Generate the experiment configurations and build one ChampSim binary per (variant, policy) inside WSL.
#
# Same approach as ../wsl-build.sh (mirror into ext4, normalize CRLF, build there), except that every
# experiments/configs/*.json is configured at once with "--join chain", which yields one executable per file
# (bin/champsim_<variant>_<policy>). The default "product" join would merge the files into a single build.
#
# Usage, from a WSL shell in the ChampSim directory:
#   bash experiments/build.sh                                       # all variants x lru srrip drrip
#   bash experiments/build.sh --policies lru srrip souza_freitas    # arguments go to gen_configs.py
#   bash experiments/build.sh --variants p1 p2s --policies lru
#
# Environment:
#   CHAMPSIM_BUILD_DIR   mirror location (default ~/champsim-build)
#   MAKE_JOBS            parallel make jobs (default nproc)

set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/"
DST="${CHAMPSIM_BUILD_DIR:-$HOME/champsim-build}/"

python3 "$SRC/experiments/gen_configs.py" "$@"

# vcpkg_installed is excluded so --delete does not wipe dependencies already installed in the mirror.
mkdir -p "$DST"
rsync -a --delete \
  --exclude .git \
  --exclude 'vcpkg/buildtrees' --exclude 'vcpkg/downloads' --exclude 'vcpkg/packages' --exclude vcpkg_installed \
  --exclude .csconfig --exclude _configuration.mk --exclude absolute.options --exclude bin --exclude 'test/bin' \
  "$SRC" "$DST"

cd "$DST"
sed -i 's/\r$//' global.options module.options config.sh Makefile

if [ ! -d vcpkg_installed ]; then
  ./vcpkg/bootstrap-vcpkg.sh -disableMetrics
  ./vcpkg/vcpkg install
fi

python3 config.sh --join chain experiments/configs/*.json
make -j"${MAKE_JOBS:-$(nproc)}"

echo
echo "Built in $DST"
ls -la "$DST/bin" 2>/dev/null || true
