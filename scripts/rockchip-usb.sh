# Shared helper for flash-emmc.sh / backup-emmc.sh / flash-at.sh / the eMMC check scripts (sourced, not executed).
# Expects: RKDT, LOADER, SUDO, die().

# Check that a Rockchip device is on the USB bus (and lsusb is installed).
check_usb() {
    command -v lsusb >/dev/null || die "lsusb (usbutils) is missing on this machine: install it"
    lsusb -d 2207: >/dev/null 2>&1 || die "no Rockchip USB device (VID 2207) on the bus: put the board into maskrom mode (see docs/FLASHING.md)"
}
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

# Write a file in sector-aligned chunks, then verify the entire file (including any resumed prefix).
write_and_verify() (
    local file=$1 lba=$2 offset=$3 chunk=$4 skip_board_mib=$5
    local size sectors scratch n sector try output
    size=$(stat -c %s "$file")
    sectors=$(( (size + 511) / 512 ))
    (( chunk > 0 && offset >= 0 && offset <= sectors && size > 0 && lba + sectors <= 4294967296 )) || die "invalid write range"
    scratch=$(mktemp -d)
    trap 'rm -rf "$scratch"' EXIT
    while (( offset < sectors )); do
        n=$((sectors - offset)); if (( n > chunk )); then n=$chunk; fi
        sector=$((lba + offset))
        dd if="$file" of="$scratch/piece" bs=1M iflag=skip_bytes,count_bytes skip="$((offset * 512))" count="$((n * 512))" status=none
        for try in 1 2 3; do
            if output=$($SUDO "$RKDT" wl "$sector" "$scratch/piece" 2>&1) && [[ ${output,,} != *fail* ]]; then break; fi
            echo "write attempt $try failed at sector $sector: $output" >&2
            if (( try == 3 )); then
                (( skip_board_mib )) && echo "Resume with --start-mb $((offset / 2048))" >&2
                die "write failed at sector $sector"
            fi
            sleep 2
        done
        offset=$((offset + n))
        echo "written $offset / $sectors sectors"
    done
    output=$($SUDO "$RKDT" rl "$lba" "$sectors" "$scratch/back" 2>&1) && [[ ${output,,} != *fail* ]] || die "read-back failed: $output"
    [[ -f "$scratch/back" && $(stat -c %s "$scratch/back") -ge $size ]] || die "short read-back"
    if (( skip_board_mib && size > 3 * 1048576 )); then
        # MiB 3 of a full image is rewritten by the board itself.
        cmp -n $((3 * 1048576)) "$file" "$scratch/back" || die "read-back differs before MiB 3"
        if (( size > 4 * 1048576 )); then
            cmp -i $((4 * 1048576)) -n $((size - 4 * 1048576)) "$file" "$scratch/back" || die "read-back differs after MiB 3"
        fi
    else
        cmp -n "$size" "$file" "$scratch/back" || die "read-back differs"
    fi
    echo "read-back verified"
)
