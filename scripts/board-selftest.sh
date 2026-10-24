#!/usr/bin/env bash
# Stress and health test that runs ON the board, as root.
# Normally started from the PC by scripts/test-board.sh.
#   DURATION=60            seconds per load phase (default 60)
#   IPERF_SERVER=<ip>      PC running "iperf3 -s"; the network phase is skipped without it
#   MAX_TEMP=85            fail above this temperature, degrees C
# Exit status: 0 = no FAIL, 1 = at least one FAIL.
set -u
D=${DURATION:-60}
SRV=${IPERF_SERVER:-}
MAX_TEMP=${MAX_TEMP:-85}
W=/var/tmp/selftest
rm -rf "$W"; mkdir -p "$W"
export PATH=$PATH:/usr/sbin:/sbin

RES=()
res() { RES+=("$1|$2|$3"); printf '[%-4s] %s: %s\n' "$2" "$1" "$3"; }
hdr() { printf '\n=== %s ===\n' "$*"; }

need() {   # need <command> <package>
    command -v "$1" >/dev/null 2>&1 && return 0
    [ -n "${APT_UPDATED:-}" ] || { apt-get update -qq >/dev/null 2>&1; APT_UPDATED=1; }
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$2" >/dev/null 2>&1
    command -v "$1" >/dev/null 2>&1
}

temp_max() {   # highest thermal zone, degrees C
    cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -n | tail -1 | awk '{printf "%d", $1/1000}'
}
freq_of() {    # current frequency of a cpu in MHz, or n/a
    local f=/sys/devices/system/cpu/cpu$1/cpufreq/scaling_cur_freq
    [ -r "$f" ] && awk '{printf "%d", $1/1000}' "$f" || echo n/a
}
gpu_freq() {
    local f
    for f in /sys/class/devfreq/*gpu*/cur_freq; do
        [ -r "$f" ] && { awk '{printf "%d", $1/1000000}' "$f"; return; }
    done
    echo n/a
}

# Samples temperature and frequencies every 5 s into $W/sample.log until killed.
sampler_start() {
    ( while :; do
        printf '%s temp=%s little=%s big=%s gpu=%s\n' "$(date +%T)" "$(temp_max)" "$(freq_of 0)" "$(freq_of 4)" "$(gpu_freq)"
        sleep 5
    done ) >> "$W/sample.log" &
    SAMPLER=$!
}
sampler_stop() { kill "$SAMPLER" 2>/dev/null; wait "$SAMPLER" 2>/dev/null; }

BAD='Call trace|BUG:|Oops|rcu.*stall|hung task|Out of memory|oom-kill|segfault|I/O error|under-?voltage|thermal.*(critical|shutdown)|panfrost.*(error|fault|timeout)|rtw.*(error|timeout|failed)|Bluetooth.*(failed|timeout)|mmc[01]:.*(error|timeout)|regulator.*(fail|error)|EXT4-fs error'
dmesg_new() {   # dmesg lines since the baseline, filtered for real problems
    dmesg | tail -n +"$((DMESG_BASE + 1))" | grep -Ei "$BAD" | grep -v 'mmc2\|fe310000'
}

hdr "Board"
echo "kernel : $(uname -r)"
echo "uptime : $(uptime -p)"
echo "cpus   : $(nproc)   temp: $(temp_max) C   little: $(freq_of 0) MHz   big: $(freq_of 4) MHz   gpu: $(gpu_freq) MHz"
echo "memory : $(free -m | awk '/Mem:/{print $2" MiB total, "$7" MiB available"}')"
DMESG_BASE=$(dmesg | wc -l)

hdr "Tools"
for t in "stress-ng stress-ng" "iperf3 iperf3" "eglinfo mesa-utils" "glmark2-es2-drm glmark2-es2-drm" "iw iw" "bluetoothctl bluez" "ethtool ethtool" "modetest libdrm-tests" "aplay alsa-utils" "lsusb usbutils" "gpioinfo gpiod" "i2cdetect i2c-tools" "clinfo clinfo" "v4l2-ctl v4l-utils" "media-ctl v4l-utils" "ffmpeg ffmpeg"; do
    set -- $t
    need "$1" "$2" && echo "ok      $1" || echo "MISSING $1 (package $2)"
done

hdr "Static checks"
st=$(systemctl is-system-running 2>&1)
failed=$(systemctl --failed --no-legend | wc -l)
[ "$failed" = 0 ] && res "systemd units" PASS "state=$st, no failed units" || res "systemd units" FAIL "state=$st, $failed failed: $(systemctl --failed --no-legend | awk '{print $2}' | tr '\n' ' ')"
if [ -e /dev/dri/renderD128 ]; then res "GPU device" PASS "/dev/dri/renderD128"; else res "GPU device" FAIL "no /dev/dri/renderD128 (vdd_gpu regulator or panfrost not probed)"; fi
if [ -e /sys/devices/system/cpu/cpu0/cpufreq ]; then res "cpufreq" PASS "little $(freq_of 0) MHz, big $(freq_of 4) MHz"; else res "cpufreq" FAIL "no cpufreq (vdd_cpu_b regulator not probed)"; fi
n=$(ip -br link | grep -c '^wl'); [ "$n" -ge 1 ] && res "Wi-Fi interface" PASS "$(ip -br link | awk '/^wl/{print $1}' | head -1)" || res "Wi-Fi interface" FAIL "no wlan interface"
b=$(ls /sys/class/bluetooth 2>/dev/null | grep -c '^hci[0-9]*$'); [ "$b" -ge 1 ] && res "Bluetooth adapter" PASS "$(ls /sys/class/bluetooth | grep '^hci[0-9]*$' | head -1)" || res "Bluetooth adapter" FAIL "no hci device"

hdr "USB (plug a device into every port before running; USB 3 devices show 5000M)"
lsusb -t 2>/dev/null | sed 's/^/  /'
nb=$(lsusb | grep -c 'root hub')
x=$(ls -d /sys/bus/platform/drivers/xhci-hcd/*.auto 2>/dev/null | wc -l)
[ "$x" -ge 2 ] && res "USB 3 controllers" PASS "$x xHCI controllers, $nb root hubs" || res "USB 3 controllers" FAIL "$x xHCI controllers (expected 2)"
devs=0; ss=0
for d in /sys/bus/usb/devices/[0-9]*-[0-9]*; do
    [ -r "$d/product" ] || continue
    case "$(cat "$d/bDeviceClass" 2>/dev/null)" in 09) continue;; esac   # skip hubs
    sp=$(cat "$d/speed" 2>/dev/null); devs=$((devs + 1)); [ "$sp" = 5000 ] && ss=$((ss + 1))
    printf '  port %-8s %-30s %s Mbit/s\n' "$(basename "$d")" "$(cat "$d/product")" "$sp"
done
[ "$devs" -gt 0 ] && res "USB devices" PASS "$devs device(s) attached, $ss at SuperSpeed" || res "USB devices" SKIP "nothing attached except hubs / the on-board Bluetooth"
usbdisk=$(for b in /sys/block/sd*; do [ -e "$b" ] && readlink -f "$b" | grep -q '/usb' && basename "$b"; done | head -1)
if [ -n "$usbdisk" ]; then
    r=$(dd if=/dev/$usbdisk of=/dev/null bs=1M count=256 iflag=direct 2>&1 | tail -1)
    [ -n "$r" ] && res "USB storage read" PASS "/dev/$usbdisk: $r" || res "USB storage read" FAIL "/dev/$usbdisk unreadable"
else res "USB storage read" SKIP "no USB disk attached"; fi

hdr "Display (HDMI; DisplayPort over USB-C is not supported yet)"
cards=$(ls -d /sys/class/drm/card[0-9] 2>/dev/null | wc -l)
if [ "$cards" = 0 ]; then
    res "Display (DRM)" FAIL "no DRM card (rockchip-drm did not bind)"
else
    hd=$(ls -d /sys/class/drm/card*-HDMI-A-* 2>/dev/null | head -1)
    if [ -n "$hd" ]; then
        st=$(cat "$hd/status"); modes=$(cat "$hd/modes" 2>/dev/null | head -1)
        if [ "$st" = connected ]; then res "HDMI" PASS "connected, preferred mode ${modes:-?}"
        else res "HDMI" SKIP "connector present, no monitor connected"; fi
    else res "HDMI" FAIL "DRM card without an HDMI connector"; fi
    dp=$(ls -d /sys/class/drm/card*-DP-* 2>/dev/null | head -1)
    [ -n "$dp" ] && res "DisplayPort" PASS "$(basename "$dp"): $(cat "$dp/status")" || res "DisplayPort" SKIP "not supported yet (cdn-dp needs a Type-C extcon, docs/OPEN.md)"
fi

hdr "Audio"
if command -v aplay >/dev/null; then
    aplay -l 2>/dev/null | grep '^card' | sed 's/^/  /'
    n=$(aplay -l 2>/dev/null | grep -c '^card')
    [ "$n" -ge 1 ] && res "Sound cards" PASS "$n playback device(s)" || res "Sound cards" FAIL "no playback device"
    while read -r c name; do
        [ -n "$c" ] || continue
        if timeout 15 speaker-test -D "plughw:$c" -t sine -f 440 -c 2 -l 1 >/dev/null 2>"$W/audio-$c.log"; then
            res "Audio out card $c" PASS "$name: tone played without errors (listen at the jack / HDMI monitor)"
        else res "Audio out card $c" FAIL "$name: $(tail -1 "$W/audio-$c.log")"; fi
    done < <(aplay -l 2>/dev/null | sed -n 's/^card \([0-9]*\): \([^,]*\),.*/\1 \2/p' | sort -u)
    if arecord -l 2>/dev/null | grep -q '^card'; then
        timeout 10 arecord -D plughw:0 -d 2 -f S16_LE -r 48000 -c 2 /dev/null >/dev/null 2>&1 \
            && res "Audio capture" PASS "2 s recorded from card 0" || res "Audio capture" FAIL "arecord failed on card 0"
    fi
else res "Audio" SKIP "alsa-utils not available"; fi

hdr "Hardware video codecs"
if [ -e /dev/mpp_service ]; then
    # Rockchip mpp_service (device tree variant -mpp): H.264 encode and decode of a generated test clip
    if command -v mpi_enc_test >/dev/null && command -v ffmpeg >/dev/null; then
        in=$W/in.yuv; out=$W/out.h264; dec=$W/dec.yuv
        ffmpeg -v error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -frames:v 60 -pix_fmt nv12 -f rawvideo "$in"
        mpi_enc_test -i "$in" -w 1280 -h 720 -f 0 -t 7 -n 60 -o "$out" -v f > "$W/enc.log" 2>&1; rc=$?
        fps=$(grep -o 'fps avg *[0-9.]*' "$W/enc.log" | tail -1 | awk '{print $3}')
        frames=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$out" 2>/dev/null)
        if [ $rc = 0 ] && [ "${frames:-0}" = 60 ]; then res "H.264 encode (mpp)" PASS "720p, 60 frames in, ${frames} out, ${fps:-?} fps, $(stat -c %s "$out") bytes"
        else res "H.264 encode (mpp)" FAIL "exit $rc, frames out=${frames:-0} (see $W/enc.log)"; fi
        mpi_dec_test -i "$out" -t 7 -n 60 -o "$dec" -v f > "$W/dec.log" 2>&1; rc=$?
        sz=$(stat -c %s "$dec" 2>/dev/null || echo 0)
        if [ $rc = 0 ] && [ "$sz" -ge $((60 * 1280 * 720 * 3 / 2 * 9 / 10)) ]; then res "H.264 decode (mpp)" PASS "decoded $sz bytes of raw video"
        else res "H.264 decode (mpp)" FAIL "exit $rc, output $sz bytes (see $W/dec.log)"; fi
        rm -f "$in" "$dec"
    else res "H.264 encode (mpp)" SKIP "mpi_enc_test or ffmpeg missing"; fi
else
    vd=$(v4l2-ctl --list-devices 2>/dev/null | grep -ciE 'rkvdec|hantro|vpu')
    [ "${vd:-0}" -gt 0 ] && res "Video decode (V4L2)" PASS "$vd V4L2 codec device(s), stateless API; use the -mpp variant for encoding" || res "Video decode (V4L2)" FAIL "no rkvdec/hantro device"
    res "H.264 encode (mpp)" SKIP "needs the -mpp device tree: sudo tinker-boot-select mpp, reboot"
fi

hdr "40-pin header"
# pin:gpiochip:line:name  (plain GPIO pins; docs/GPIO.md)
for e in 7:2:25:GPIO2_D1 11:2:19:GPIO2_C3 12:3:24:GPIO3_D0 22:3:28:GPIO3_D4 29:3:30:GPIO3_D6 31:3:29:GPIO3_D5 \
         35:3:25:GPIO3_D1 36:2:18:GPIO2_C2 37:4:21:GPIO4_C5 38:3:27:GPIO3_D3 40:3:31:GPIO3_D7; do
    IFS=: read -r pin chip line name <<< "$e"
    if ! command -v gpioset >/dev/null; then res "GPIO pin $pin" SKIP "gpiod tools missing"; continue; fi
    ok=1
    timeout 1 gpioset -c "gpiochip$chip" "$line=1" >/dev/null 2>&1; r=$?; [ $r = 0 ] || [ $r = 124 ] || ok=0
    timeout 1 gpioset -c "gpiochip$chip" "$line=0" >/dev/null 2>&1; r=$?; [ $r = 0 ] || [ $r = 124 ] || ok=0
    gpioget -c "gpiochip$chip" "$line" >/dev/null 2>&1 || ok=0
    [ $ok = 1 ] && res "GPIO pin $pin ($name)" PASS "gpiochip$chip line $line: output high/low and input work" || res "GPIO pin $pin ($name)" FAIL "gpiochip$chip line $line cannot be driven (busy or not a GPIO)"
done
# optional wire loopback: GPIO_LOOPBACK="7:29 31:37" (pairs of pins joined with a jumper wire)
declare -A PIN=( [7]=2:25 [11]=2:19 [12]=3:24 [22]=3:28 [29]=3:30 [31]=3:29 [35]=3:25 [36]=2:18 [37]=4:21 [38]=3:27 [40]=3:31 )
for pair in ${GPIO_LOOPBACK:-}; do
    a=${pair%%:*}; b=${pair##*:}; IFS=: read -r ca la <<< "${PIN[$a]:-}"; IFS=: read -r cb lb <<< "${PIN[$b]:-}"
    [ -n "${ca:-}" ] && [ -n "${cb:-}" ] || { res "GPIO loopback $pair" FAIL "pins must be plain GPIO pins"; continue; }
    good=1
    for v in 1 0 1; do
        gpioset -c "gpiochip$ca" "$la=$v" >/dev/null 2>&1 & gp=$!; sleep 0.3
        got=$(gpioget -c "gpiochip$cb" -p none "$lb" 2>/dev/null | grep -o 'active\|inactive' | head -1)
        kill $gp 2>/dev/null; wait $gp 2>/dev/null
        [ "$got" = "$([ $v = 1 ] && echo active || echo inactive)" ] || good=0
    done
    [ $good = 1 ] && res "GPIO loopback $pair" PASS "pin $a drives pin $b high and low" || res "GPIO loopback $pair" FAIL "pin $b did not follow pin $a (wire connected?)"
done
[ -z "${GPIO_LOOPBACK:-}" ] && res "GPIO loopback" SKIP "no wires: run with GPIO_LOOPBACK=\"7:29 31:37\" and jumper wires between those pin pairs"
for b in 6 7; do
    if [ -e /dev/i2c-$b ] && i2cdetect -y "$b" >/dev/null 2>&1; then res "I2C$b (/dev/i2c-$b)" PASS "bus scan works: $(i2cdetect -y "$b" 2>/dev/null | grep -c '[0-9a-f][0-9a-f] *$' ) device row(s)"
    else res "I2C$b (/dev/i2c-$b)" FAIL "bus missing or scan failed"; fi
done
for d in spidev1.0 spidev5.0; do
    [ -e /dev/$d ] && res "SPI (/dev/$d)" PASS "device node present" || res "SPI (/dev/$d)" FAIL "missing (module spidev loaded?)"
done
npwm=0
for c in /sys/class/pwm/pwmchip*; do
    [ -e "$c" ] || continue
    if echo 0 > "$c/export" 2>/dev/null; then
        echo 1000000 > "$c/pwm0/period" && echo 500000 > "$c/pwm0/duty_cycle" && echo 1 > "$c/pwm0/enable" \
            && [ "$(cat "$c/pwm0/enable")" = 1 ] && npwm=$((npwm + 1))
        echo 0 > "$c/pwm0/enable" 2>/dev/null; echo 0 > "$c/unexport" 2>/dev/null
    fi
done
[ "$npwm" -ge 3 ] && res "PWM (pins 26, 32, 33)" PASS "$npwm PWM channels start and stop" || res "PWM (pins 26, 32, 33)" FAIL "only $npwm of 3 channels work"

hdr "MIPI cameras and displays"
cam=$(for m in /dev/media*; do [ -e "$m" ] && media-ctl -d "$m" -p 2>/dev/null | grep -m1 'driver ' | grep -qi 'rkisp1' && echo "$m"; done)
[ -n "$cam" ] && res "MIPI camera" PASS "rkisp1 media device: $(echo $cam | tr '\n' ' ')" \
    || res "MIPI camera" SKIP "no camera described in the device tree (docs/MIPI.md)"
dsi=$(ls -d /sys/class/drm/card*-DSI-* 2>/dev/null | head -1)
[ -n "$dsi" ] && res "MIPI DSI display" PASS "$(basename "$dsi"): $(cat "$dsi/status")" || res "MIPI DSI display" SKIP "no DSI panel described in the device tree (docs/MIPI.md)"

hdr "RTC"
if [ -e /dev/rtc0 ]; then
    rn=$(cat /sys/class/rtc/rtc0/name 2>/dev/null)
    hwclock --systohc 2>/dev/null; r=$?
    rt=$(hwclock -r --utc 2>/dev/null | head -1); rs=$(date -d "$rt" +%s 2>/dev/null || echo 0); now=$(date +%s)
    d=$(( rs > now ? rs - now : now - rs ))
    if [ $r = 0 ] && [ "$d" -le 5 ]; then res "RTC" PASS "$rn: written from the system clock, read back within ${d}s (time survives power-off only with a battery on the RTC_BATTERY header)"
    else res "RTC" FAIL "$rn: hwclock exit $r, differs from the system clock by ${d}s"; fi
else res "RTC" FAIL "no /dev/rtc0"; fi

sampler_start

hdr "CPU, all cores, ${D}s (matrix product, results verified)"
if command -v stress-ng >/dev/null; then
    stress-ng --cpu 0 --cpu-method matrixprod --verify --metrics-brief -t "${D}s" > "$W/cpu.log" 2>&1; rc=$?
    grep -E 'bogo|cpu ' "$W/cpu.log" | tail -2
    [ $rc = 0 ] && res "CPU all cores" PASS "${D}s, verified" || res "CPU all cores" FAIL "stress-ng exit $rc (see $W/cpu.log)"
    if [ "$(nproc)" = 6 ]; then
        stress-ng --taskset 0-3 --cpu 4 --cpu-method matrixprod --verify -t 20s > "$W/cpu-little.log" 2>&1; r1=$?
        stress-ng --taskset 4-5 --cpu 2 --cpu-method matrixprod --verify -t 20s > "$W/cpu-big.log" 2>&1; r2=$?
        [ $r1 = 0 ] && res "CPU Cortex-A53 (0-3)" PASS "20s, verified" || res "CPU Cortex-A53 (0-3)" FAIL "exit $r1"
        [ $r2 = 0 ] && res "CPU Cortex-A72 (4-5)" PASS "20s, verified" || res "CPU Cortex-A72 (4-5)" FAIL "exit $r2"
    fi
else res "CPU" SKIP "stress-ng not available"; fi

hdr "RAM, ~70% of memory, ${D}s (patterns written and verified)"
if command -v stress-ng >/dev/null; then
    stress-ng --vm 2 --vm-bytes 70% --vm-method all --verify --metrics-brief -t "${D}s" > "$W/vm.log" 2>&1; rc=$?
    grep -E 'bogo|vm ' "$W/vm.log" | tail -2
    [ $rc = 0 ] && res "RAM" PASS "${D}s, verified" || res "RAM" FAIL "stress-ng exit $rc (see $W/vm.log)"
else res "RAM" SKIP "stress-ng not available"; fi

hdr "eMMC, write and read back 512 MiB"
crc0=$(grep -E 'Data (CRC|Timeout)' /sys/kernel/debug/mmc1/err_stats 2>/dev/null | awk '{s+=$NF} END{print s+0}')
f=$W/io.bin
dd if=/dev/urandom of="$f.src" bs=1M count=64 status=none
for i in 1 2 3 4 5 6 7 8; do cat "$f.src"; done > "$f"
want=$(sha256sum "$f" | cut -d' ' -f1)
t0=$(date +%s.%N); sync; cp "$f" "$f.copy"; sync; t1=$(date +%s.%N)
echo 3 > /proc/sys/vm/drop_caches
t2=$(date +%s.%N); got=$(sha256sum "$f.copy" | cut -d' ' -f1); t3=$(date +%s.%N)
wr=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.0f", 512/(b-a)}'); rd=$(awk -v a="$t2" -v b="$t3" 'BEGIN{printf "%.0f", 512/(b-a)}')
crc1=$(grep -E 'Data (CRC|Timeout)' /sys/kernel/debug/mmc1/err_stats 2>/dev/null | awk '{s+=$NF} END{print s+0}')
if [ "$want" != "$got" ]; then res "eMMC" FAIL "checksum mismatch"
elif [ "$wr" -lt 10 ] || [ "$rd" -lt 20 ]; then res "eMMC" FAIL "too slow: write ~${wr} MiB/s, read ~${rd} MiB/s (expected 40+ and 100+; bus timing / CRC errors?)"
elif [ "${crc1:-0}" -gt "${crc0:-0}" ]; then res "eMMC" FAIL "$((crc1 - crc0)) new data CRC/timeout errors during the test"
else res "eMMC" PASS "checksum ok, write ~${wr} MiB/s, read ~${rd} MiB/s, no bus errors"; fi
rm -f "$f" "$f.src" "$f.copy"

hdr "GPU (panfrost)"
if [ ! -e /dev/dri/renderD128 ]; then res "GPU renderer" FAIL "no /dev/dri/renderD128"
elif ! command -v eglinfo >/dev/null; then res "GPU renderer" SKIP "eglinfo missing (package mesa-utils)"
else
    r=$(eglinfo -p gbm -B 2>/dev/null | grep -i 'renderer' | grep -i -m1 'panfrost\|mali')
    [ -n "$r" ] && res "GPU renderer" PASS "$(echo "$r" | sed 's/^ *//')" || res "GPU renderer" FAIL "no Panfrost/Mali renderer in eglinfo (software rendering?)"
    if command -v glmark2-es2-drm >/dev/null; then
        glmark2-es2-drm --off-screen -b build:duration=10 -b texture:duration=10 -b shading:duration=10 > "$W/glmark.log" 2>&1; rc=$?
        score=$(grep -i 'glmark2 Score' "$W/glmark.log" | grep -o '[0-9]*' | tail -1)
        if [ $rc = 0 ] && [ -n "$score" ]; then res "GPU load (glmark2)" PASS "score $score, gpu $(gpu_freq) MHz"
        else res "GPU load (glmark2)" SKIP "glmark2 needs a connected display (HDMI): $(tail -1 "$W/glmark.log")"; fi
    fi
fi

hdr "OpenCL (Rusticl on Panfrost)"
if command -v clinfo >/dev/null; then
    cn=$(RUSTICL_ENABLE=panfrost clinfo 2>/dev/null | awk '/Platform Name/{p=$0} /Device Name/{print p" | "$0}' | grep -i rusticl | grep -ci 'panfrost\|mali')
    [ "${cn:-0}" -ge 1 ] && res "OpenCL" PASS "Mali-T860 visible through Rusticl (OpenCL 3.0)" || res "OpenCL" FAIL "no Panfrost device in Rusticl (RUSTICL_ENABLE=panfrost, package mesa-opencl-icd)"
else res "OpenCL" SKIP "clinfo missing"; fi

hdr "Network (gigabit, 2 x ${D}s)"
link=$(ethtool end0 2>/dev/null | awk '/Speed:/{print $2}')
echo "link: ${link:-unknown}"
if [ -n "$SRV" ] && command -v iperf3 >/dev/null; then
    iperf3 -c "$SRV" -t "$D" -J > "$W/net-up.json" 2>&1; r1=$?
    iperf3 -c "$SRV" -t "$D" -R -J > "$W/net-down.json" 2>&1; r2=$?
    up=$(python3 -I -c 'import json,sys;d=json.load(open(sys.argv[1]));e=d["end"];print("%d Mbit/s, %d retransmits"%(e["sum_sent"]["bits_per_second"]/1e6,e["sum_sent"].get("retransmits",0)))' "$W/net-up.json" 2>/dev/null)
    dn=$(python3 -I -c 'import json,sys;d=json.load(open(sys.argv[1]));e=d["end"];print("%d Mbit/s"%(e["sum_received"]["bits_per_second"]/1e6))' "$W/net-down.json" 2>/dev/null)
    [ $r1 = 0 ] && [ -n "$up" ] && res "Network upload" PASS "$up (link $link)" || res "Network upload" FAIL "iperf3 exit $r1"
    [ $r2 = 0 ] && [ -n "$dn" ] && res "Network download" PASS "$dn (link $link)" || res "Network download" FAIL "iperf3 exit $r2"
else res "Network" SKIP "no IPERF_SERVER (start iperf3 -s on the PC)"; fi

hdr "Wi-Fi and Bluetooth scan"
wl=$(ip -br link | awk '/^wl/{print $1}' | head -1)
if [ -n "$wl" ] && command -v iw >/dev/null; then
    ip link set "$wl" up 2>/dev/null; sleep 2
    n=$(iw dev "$wl" scan 2>/dev/null | grep -c '^BSS')
    [ "$n" -gt 0 ] && res "Wi-Fi scan" PASS "$n access points seen" || res "Wi-Fi scan" FAIL "no access points (antenna connected? 2.4/5 GHz networks nearby?)"
fi
if command -v bluetoothctl >/dev/null; then
    systemctl start bluetooth 2>/dev/null; sleep 2
    n=$(timeout 15 bluetoothctl --timeout 10 scan on 2>/dev/null | grep -c 'Device ')
    [ "$n" -gt 0 ] && res "Bluetooth scan" PASS "$n devices seen" || res "Bluetooth scan" SKIP "no devices seen (none nearby, or no advertising devices)"
fi

hdr "Soak: CPU + RAM + GPU + network at the same time, ${D}s"
pids=()
if command -v stress-ng >/dev/null; then
    stress-ng --cpu 0 --cpu-method matrixprod --vm 1 --vm-bytes 50% --vm-method all --verify -t "${D}s" > "$W/soak.log" 2>&1 &
    pids+=($!); sp=$!
fi
if [ -n "$SRV" ] && command -v iperf3 >/dev/null; then iperf3 -c "$SRV" -t "$D" --bidir -J > "$W/soak-net.json" 2>&1 & pids+=($!); np=$!; fi
if [ -e /dev/dri/renderD128 ] && command -v glmark2-es2-drm >/dev/null; then
    timeout "$D" glmark2-es2-drm --off-screen --run-forever > "$W/soak-gpu.log" 2>&1 & pids+=($!)
fi
src=0; nrc=0
[ -n "${sp:-}" ] && { wait "$sp"; src=$?; }
[ -n "${np:-}" ] && { wait "$np"; nrc=$?; }
for p in "${pids[@]}"; do wait "$p" 2>/dev/null; done
[ $src = 0 ] && [ $nrc = 0 ] && res "Soak" PASS "all loads at once for ${D}s, no errors" || res "Soak" FAIL "stress-ng exit $src, iperf3 exit $nrc"

sampler_stop

hdr "Power profiles (tinker-power)"
if command -v tinker-power >/dev/null; then
    cur() { tinker-power status | sed -n 's/^profile now: *\([a-z]*\).*/\1/p'; }
    orig=$(cur); bad=""
    for pr in powersave performance normal; do
        tinker-power "$pr" --no-save >/dev/null 2>&1; got=$(cur)
        [ "$got" = "$pr" ] || bad="$bad $pr(got:$got)"
    done
    case "$orig" in powersave|normal|performance) tinker-power "$orig" --no-save >/dev/null 2>&1 ;; *) tinker-power normal --no-save >/dev/null 2>&1 ;; esac
    [ -z "$bad" ] && res "Power profiles" PASS "powersave, performance and normal apply and are recognised; restored '$orig'" || res "Power profiles" FAIL "profile did not stick:$bad"
    if systemctl is-enabled tinker-power >/dev/null 2>&1 && [ -f /etc/udev/rules.d/61-tinker-power.rules ]; then
        res "Power profile at boot" PASS "tinker-power.service enabled and the GPU udev rule present, saved profile: $(sed -n 's/^PROFILE=//p' /etc/tinker-power.conf)"
    else res "Power profile at boot" FAIL "tinker-power.service not enabled or the udev rule is missing"; fi
else res "Power profiles" SKIP "tinker-power not installed"; fi

hdr "Health after the tests"
tm=$(awk -F'temp=' '{split($2,a," "); if (a[1]+0>m) m=a[1]+0} END{print m+0}' "$W/sample.log")
lmin=$(awk '{for(i=1;i<=NF;i++) if ($i ~ /^little=[0-9]+$/) {split($i,a,"="); if (min==""||a[2]<min) min=a[2]; if (a[2]>max) max=a[2]}} END{print (min==""?"n/a":min"-"max)}' "$W/sample.log")
[ "${tm:-0}" -lt "$MAX_TEMP" ] && res "Temperature" PASS "max ${tm} C (limit ${MAX_TEMP}); little cluster ${lmin} MHz during the run" || res "Temperature" FAIL "max ${tm} C >= ${MAX_TEMP}"
bad=$(dmesg_new)
[ -z "$bad" ] && res "Kernel log" PASS "no errors since the start of the test" || { res "Kernel log" FAIL "$(echo "$bad" | wc -l) suspicious lines"; echo "$bad" | head -20; }
echo "last samples:"; tail -3 "$W/sample.log"

hdr "Summary"
fails=0
for r in "${RES[@]}"; do
    IFS='|' read -r n s d <<< "$r"
    printf '%-6s %-22s %s\n' "$s" "$n" "$d"
    [ "$s" = FAIL ] && fails=$((fails + 1))
done
echo
[ "$fails" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL ($fails)"
[ "$fails" = 0 ]
