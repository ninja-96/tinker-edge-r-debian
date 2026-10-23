# Shared helper for flash-emmc.sh / backup-emmc.sh (sourced, not executed).
# Expects: RKDT, LOADER, SUDO, die().
#
# `rkdeveloptool ld` keeps printing "Maskrom" even while the downloaded loader
# is running, and a second `db` into a running loader hangs. So the loader state
# is detected by asking the board for its flash info instead.

loader_running() {
    local out
    out=$(timeout --foreground 15 $SUDO "$RKDT" rfi 2>&1) || true
    echo "$out" | grep -q "Flash Size"
}

# Make sure the loader runs on the board (download it only when needed).
load_loader() {
    local try rc i
    # `timeout` starts its command in the background, where sudo cannot ask for a
    # password: ask for it now, once, so that the calls below find it cached.
    [ -z "${SUDO:-}" ] || $SUDO -v || die "sudo failed"
    if loader_running; then
        echo "loader already running on the board, skipping db"
        return 0
    fi
    for try in 1 2 3; do
        rc=0
        timeout --foreground 60 $SUDO "$RKDT" db "$LOADER" || rc=$?
        echo "db attempt $try: exit code $rc"
        if [ "$rc" = 0 ]; then
            for i in $(seq 1 15); do
                sleep 1
                if loader_running; then echo "loader is running"; return 0; fi
            done
            echo "the board does not answer after db" >&2
        fi
        if [ "$rc" = 124 ]; then
            echo "db timed out: the board is stuck in the BootROM transfer" >&2
            break
        fi
        sleep 2
    done
    die "could not start the loader on the board. Power-cycle it into maskrom (unplug power and USB, J3 shorted, plug USB, then power), use a USB 2.0 port directly on the PC (no hub), and retry"
}
