#!/usr/bin/env bash
# Desktop diagnostics, run ON the board as the user who is logged in to the desktop
# (no sudo needed). Answers: which compositor and renderer is used, which resolution,
# at which clock the CPU and the GPU run while windows are moved, and which processes eat CPU.
#   scripts/diag-desktop.sh [SECONDS]        default 20
# Start it, then drag windows around for the whole time. The report goes to the screen and
# to ~/desktop-diag.txt.
set -u
N=${1:-20}
OUT=$HOME/desktop-diag.txt
exec > >(tee "$OUT") 2>&1
export PATH=$PATH:/usr/sbin:/sbin

hdr() { printf '\n=== %s ===\n' "$*"; }
GPU=/sys/class/devfreq/ff9a0000.gpu
cpu_mhz() { awk '{printf "%d", $1/1000}' "/sys/devices/system/cpu/cpufreq/policy$1/scaling_cur_freq"; }

hdr "System"
echo "kernel $(uname -r), $(cat /proc/device-tree/model | tr -d '\0')"
echo "uptime $(uptime -p), memory $(free -m | awk '/Mem:/{print $3" of "$2" MiB used"}'), swap $(free -m | awk '/Swap:/{print $3" of "$2" MiB"}')"

hdr "Session and compositor"
echo "XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-?}  XDG_CURRENT_DESKTOP=${XDG_CURRENT_DESKTOP:-?}"
ps -eo comm= | grep -E '^(kwin_wayland|kwin_x11|Xorg|Xwayland|plasmashell|sddm)' | sort | uniq -c | sed 's/^/  /'
for q in qdbus6 qdbus-qt6 qdbus; do command -v $q >/dev/null && { QD=$q; break; }; done
if [ -n "${QD:-}" ]; then
    $QD org.kde.KWin /KWin supportInformation 2>/dev/null | grep -iE "Compositing Type|OpenGL renderer|OpenGL vendor|OpenGL version|Driver|GLES|Rendering|Max texture|swap|vsync|Active|Output" | head -16 | sed 's/^/  /'
else
    echo "  (no qdbus: install qt6-tools-dev-tools or read System Settings > Display > Compositor)"
fi

hdr "OpenGL renderer (what an application gets)"
if command -v eglinfo >/dev/null; then eglinfo -B 2>/dev/null | grep -iE "renderer|OpenGL ES profile version|OpenGL core profile version" | head -4 | sed 's/^/  /'; fi
command -v glxinfo >/dev/null && glxinfo -B 2>/dev/null | grep -iE "renderer|direct rendering" | sed 's/^/  /'

hdr "Display"
if command -v kscreen-doctor >/dev/null; then kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | head -14 | sed 's/^/  /'; fi
command -v xrandr >/dev/null && [ -n "${DISPLAY:-}" ] && xrandr 2>/dev/null | grep -E "connected|\*" | head -4 | sed 's/^/  /'
for c in /sys/class/drm/card*-*; do [ -e "$c/status" ] && echo "  $(basename "$c"): $(cat "$c/status") $(head -1 "$c/modes" 2>/dev/null)"; done

hdr "Clocks before the test"
echo "  CPU little (A53) $(cpu_mhz 0) MHz governor $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor), big (A72) $(cpu_mhz 4) MHz governor $(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_governor)"
if [ -d "$GPU" ]; then echo "  GPU $(( $(cat $GPU/cur_freq)/1000000 )) MHz, min $(( $(cat $GPU/min_freq)/1000000 )), max $(( $(cat $GPU/max_freq)/1000000 )), governor $(cat $GPU/governor)"; fi

hdr "Sampling for ${N} s: MOVE WINDOWS NOW"
T=$(mktemp -t diagdesk.XXXXXX); G=$(mktemp -t diaggpu.XXXXXX)
for i in $(seq 1 "$N"); do
    ps -eo pcpu=,comm= --sort=-pcpu | head -6 | awk -v t=$i '{printf "%s %s %s\n", t, $1, $2}' >> "$T"
    printf '%s %s %s %s\n' "$i" "$(cpu_mhz 0)" "$(cpu_mhz 4)" "$([ -d $GPU ] && echo $(( $(cat $GPU/cur_freq)/1000000 )) || echo 0)" >> "$G"
    sleep 1
done

hdr "Result"
echo "average and peak CPU use per process (percent of one core, top six of every second):"
awk '{s[$3]+=$2; if($2>m[$3])m[$3]=$2; c[$3]++} END{for(p in s) printf "  %-22s avg %6.1f  peak %6.1f  (in the top for %d s)\n", p, s[p]/'"$N"', m[p], c[p]}' "$T" | sort -k3 -nr | head -8
echo "clocks during the test (MHz):"
awk '{a+=$2; b+=$3; g+=$4; if($4>gm)gm=$4; if(gmin==""||$4<gmin)gmin=$4; if($3>bm)bm=$3} END{printf "  A53 avg %d, A72 avg %d (peak %d), GPU avg %d (min %d, max %d)\n", a/NR, b/NR, bm, g/NR, gmin, gm}' "$G"
echo "load average: $(cut -d' ' -f1-3 /proc/loadavg)"
rm -f "$T" "$G"
echo; echo "Report saved to $OUT"
