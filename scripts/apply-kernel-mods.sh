#!/usr/bin/env bash
# Apply our out-of-tree changes to the extracted kernel tree:
#   patches/linux/*.patch   unified diffs against the pinned kernel
#   kernel-mods/            new files copied into the tree (mpp_service port)
# Idempotent. Usage: apply-kernel-mods.sh <kernel-tree>
set -euo pipefail
cd "$(dirname "$0")/.."
K=${1:?kernel tree}

for p in patches/linux/*.patch; do
    [ -e "$p" ] || continue
    if patch -d "$K" -p1 -R --dry-run -s < "$p" >/dev/null 2>&1; then
        echo "already applied: $p"
    else
        patch -d "$K" -p1 -s < "$p"
        echo "applied: $p"
    fi
done

cp -a kernel-mods/. "$K/"

# Hook the mpp directory into drivers/soc/rockchip
S=$K/drivers/soc/rockchip
if ! grep -q 'rockchip/mpp/Kconfig' "$S/Kconfig"; then
    # insert before the last "endif" (closes "if SOC_ROCKCHIP")
    python3 - "$S/Kconfig" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
i = s.rindex("endif")
s = s[:i] + 'source "drivers/soc/rockchip/mpp/Kconfig"\n\n' + s[i:]
open(p, "w").write(s)
PY
fi
grep -q 'CONFIG_ROCKCHIP_MPP_SERVICE' "$S/Makefile" ||
    echo 'obj-$(CONFIG_ROCKCHIP_MPP_SERVICE) += mpp/' >> "$S/Makefile"
grep -q 'CONFIG_TINKER_NPU_POWER' "$S/Makefile" ||
    echo 'obj-$(CONFIG_TINKER_NPU_POWER) += tinker-npu-power.o' >> "$S/Makefile"
grep -q 'config TINKER_NPU_POWER' "$S/Kconfig" ||
    python3 - "$S/Kconfig" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
i = s.rindex("endif")
s = s[:i] + """config TINKER_NPU_POWER
	tristate "ASUS Tinker Edge R RK3399Pro NPU power sequencing"
	depends on ARCH_ROCKCHIP && GPIOLIB && COMMON_CLK
	help
	  Powers up the NPU die of the RK3399Pro on the Tinker Edge R
	  (GPIO and clock sequence of the stock npu_powerctrl tool) and
	  exposes /sys/devices/platform/<npu-power>/{enable,reset}.

""" + s[i:]
open(p, "w").write(s)
PY
