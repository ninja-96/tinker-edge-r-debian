#!/usr/bin/env bash
# Use the same ARM64 interpreter setup as the standalone chroot builds.
set -euo pipefail
cd "$(dirname "$0")/.."
exec ./run.sh bash -ec '. scripts/build-chroot.sh; ensure_binfmt'
