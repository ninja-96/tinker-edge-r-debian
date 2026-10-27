#!/usr/bin/env bash
# The first step of every release: are the Linux kernel and Debian up to date?
#   scripts/check-updates.sh            report only
#   scripts/check-updates.sh --apply    write the newest kernel of the same long-term series into sources.lock (version and sha256)
# Kernel: the newest release of the series in sources.lock (6.18.x) on kernel.org, taken from releases.json; the sha256 comes from kernel.org's sha256sums.asc.
# Debian: the point release of trixie in the archive (dists/trixie/Release) against the one in out/rootfs.tar. The root file system is built from the live
# archive (mmdebstrap), so "updating Debian" means rebuilding it: ./run.sh scripts/build-rootfs.sh (see docs/RELEASING.md). Exit status 1 = something is out of date.
set -euo pipefail
cd "$(dirname "$0")/.."
LOCK=${SOURCES_LOCK:-sources.lock}
APPLY=0; [ "${1:-}" = --apply ] && APPLY=1
. "$LOCK"
stale=0

series=${LINUX_VERSION%.*}
latest=$(curl -fsS -m 30 https://www.kernel.org/releases.json | python3 -c "
import json, sys
s = sys.argv[1]
v = [r['version'] for r in json.load(sys.stdin)['releases'] if r['version'].startswith(s + '.') and r['moniker'] in ('longterm', 'stable')]
print(v[0] if v else '')" "$series")
[ -n "$latest" ] || { echo "kernel: no $series.x release found on kernel.org (is the series end of life?)" >&2; exit 2; }
if [ "$latest" = "$LINUX_VERSION" ]; then
    echo "kernel: $LINUX_VERSION is the newest of $series"
else
    echo "kernel: $LINUX_VERSION -> $latest available"
    stale=1
    if [ "$APPLY" = 1 ]; then
        tarball="linux-$latest.tar.xz"
        sum=$(curl -fsS -m 60 "https://cdn.kernel.org/pub/linux/kernel/v${latest%%.*}.x/sha256sums.asc" | awk -v f="$tarball" '$2 == f {print $1}')
        [ ${#sum} = 64 ] || { echo "kernel: no sha256 for $tarball" >&2; exit 2; }
        sed -i "s/^LINUX_VERSION=.*/LINUX_VERSION=$latest/; s/^LINUX_SHA256=.*/LINUX_SHA256=$sum/" "$LOCK"
        echo "kernel: $LOCK now has $latest ($sum). Next: scripts/fetch-sources.sh, check that patches/linux and kernel-mods apply, rebuild the kernel."
    fi
fi

rel=$(curl -fsS -m 30 https://deb.debian.org/debian/dists/trixie/Release)
arch_ver=$(echo "$rel" | awk -F': ' '$1 == "Version" {print $2}')
arch_date=$(echo "$rel" | awk -F': ' '$1 == "Date" {print $2}')
have=none
if [ -f out/rootfs.tar ]; then
    have=$(tar -xOf out/rootfs.tar ./etc/debian_version 2>/dev/null || true)
    built=$(date -r out/rootfs.tar +%Y-%m-%d)
fi
echo "debian: trixie $arch_ver in the archive (release file of $arch_date); out/rootfs.tar has ${have:-unknown}, built ${built:-never}"
if [ "$have" != "$arch_ver" ] || [ -z "${built:-}" ] || [ "$(date -d "$built" +%s)" -lt "$(date -d "$(date +%Y-%m-%d) -1 day" +%s)" ]; then
    echo "debian: rebuild the root file system (./run.sh scripts/build-rootfs.sh) so that the image has the newest packages (point release and security updates)"
    stale=1
else
    echo "debian: out/rootfs.tar is current"
fi
exit $stale
