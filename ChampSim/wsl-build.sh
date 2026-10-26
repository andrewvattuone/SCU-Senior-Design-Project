#!/usr/bin/env bash
#
# Build ChampSim from this Windows checkout inside WSL.
#
# Two things about a Windows checkout break ChampSim's native build:
#   1. The Makefile cannot handle a source path that contains spaces (this tree lives under "Senior Design"): the
#      generated -I flags split on the space and the vcpkg triplet directory resolves empty.
#   2. git's autocrlf leaves CRLF line endings, which break the "#!/usr/bin/env python3" shebang in config.sh.
#
# This script mirrors the tree into a native ext4 directory (no spaces, and far faster than /mnt/c), normalizes the
# few files that must be LF, and builds there. Build products stay in the mirror; the source of truth stays here.
#
# Usage, from a WSL shell in this directory:
#   bash wsl-build.sh                       # configure with champsim_config.json and build bin/champsim
#   bash wsl-build.sh my_config.json        # configure with another file
#   bash wsl-build.sh champsim_config.json test    # build and run the unit tests
#
# Override the mirror location with CHAMPSIM_BUILD_DIR.

set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/"
DST="${CHAMPSIM_BUILD_DIR:-$HOME/champsim-build}/"
CONFIG="${1:-champsim_config.json}"
shift || true

mkdir -p "$DST"
rsync -a --delete \
  --exclude .git --exclude vcpkg_installed \
  --exclude 'vcpkg/buildtrees' --exclude 'vcpkg/downloads' --exclude 'vcpkg/packages' \
  --exclude .csconfig --exclude _configuration.mk --exclude absolute.options --exclude bin --exclude 'test/bin' \
  "$SRC" "$DST"

cd "$DST"
sed -i 's/\r$//' global.options module.options config.sh Makefile

if [ ! -d vcpkg_installed ]; then
  ./vcpkg/bootstrap-vcpkg.sh -disableMetrics
  ./vcpkg/vcpkg install
fi

python3 config.sh "$CONFIG"
make -j"$(nproc)" "$@"

echo
echo "Built in $DST"
ls -la "$DST/bin" 2>/dev/null || true
