#!/usr/bin/env bash
# Run the stress and health test on a board over SSH (CPU, RAM, eMMC, GPU, network,
# Wi-Fi/Bluetooth, then everything at once). Details: docs/TESTING.md
#   scripts/test-board.sh [-d SECONDS] [HOST]
#   -d  seconds per load phase (default 60; the whole run takes about 8 x that)
#   HOST defaults to tinker-edge-r.local. SSH_USER (debian) and BOARD_PASS (debian,
#   used for sudo on the board) can be set in the environment. The image forces a password change
#   at the first login: log in once by hand (ssh debian@HOST), set a new password, then run this
#   script with BOARD_PASS=<new password>.
# The network phase needs iperf3 on this PC (apt install iperf3); it is started and
# stopped by this script. The PC should be on wired Ethernet for a meaningful result.
set -euo pipefail
cd "$(dirname "$0")/.."
D=60
[ "${1:-}" = -d ] && { D=${2:?seconds}; shift 2; }
HOST=${1:-tinker-edge-r.local}
U=${SSH_USER:-debian}
PASS=${BOARD_PASS:-debian}
mkdir -p out
LOG=out/test-$(date +%Y%m%d-%H%M%S).log

ctl_dir=$(mktemp -d)
CTL=$ctl_dir/ssh
SSH=(ssh -o ControlPath="$CTL" -o StrictHostKeyChecking=accept-new "$U@$HOST")
cleanup() {
    ssh -o ControlPath="$CTL" -O exit "$U@$HOST" >/dev/null 2>&1 || true
    rm -rf "$ctl_dir"
    [ -n "${IPERF_PID:-}" ] && kill "$IPERF_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "Connecting to $U@$HOST (enter the SSH password once)..."
ssh -o ControlMaster=yes -o ControlPath="$CTL" -o ControlPersist=yes -o StrictHostKeyChecking=accept-new -fN "$U@$HOST"

SRV=
if command -v iperf3 >/dev/null; then
    iperf3 -s >/dev/null 2>&1 & IPERF_PID=$!
    ip=$(getent ahostsv4 "$HOST" | awk 'NR==1{print $1}')
    SRV=$(ip -4 route get "$ip" | grep -o 'src [0-9.]*' | cut -d' ' -f2)
    echo "iperf3 server on this PC: $SRV"
else
    echo "iperf3 not installed on this PC: the network phase will be skipped"
fi

scp -q -o ControlPath="$CTL" scripts/board-selftest.sh "$U@$HOST:/tmp/board-selftest.sh"
echo "Test running, log: $LOG"
set +e
printf '%s\n' "$PASS" | "${SSH[@]}" "sudo -S -p '' env DURATION=$D IPERF_SERVER=$SRV bash /tmp/board-selftest.sh" 2>&1 | tee "$LOG"
result=("${PIPESTATUS[@]}")
rc=${result[1]}
if [ "$rc" = 0 ]; then rc=${result[2]}; fi
set -e
echo
if [ "$rc" = 0 ]; then echo "Board test: PASS ($LOG)"; else echo "Board test: FAIL, exit $rc ($LOG)"; fi
exit "$rc"
