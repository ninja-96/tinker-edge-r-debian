What remains before release 0.5 and the division of labour, agreed in the 2026-10-30 session.

Standing agreement of the session: NO commits unless the user explicitly asks (the 7 doc files of the LiteRT docs cleanup are intentionally left in the working tree; commit them only when asked).

Remaining work:
1. Repo: commit the 7 doc files (docs/{LINKS,NPU,RELEASING,ROADMAP}.md, npu-open/README.md, npu-open/host/{op_names.h,rk3399pro_npu.hpp}) when the user asks.
2. Board checks (ROADMAP "Release 0.5: hardware checks"): Wi-Fi connection + iperf3 + reboot reconnect WITH ANTENNAS; Bluetooth A2DP audio with antennas; HDMI audio by ear; USB 3 over USB-C (controller stuck in RxDetect); boot logo on the board; suspend/resume full cycle with the new npu_daemon (3dc6c07): NPU with a loaded model through deep sleep, RTC wake; final `scripts/test-board.sh` -> RESULT: PASS.
3. Release procedure per docs/RELEASING.md: `scripts/check-updates.sh --apply` (newest 6.18.x at build time, currently 6.18.59) + rootfs rebuild; check patches/kernel-mods/config apply; check pinned sources; links and licences; `scripts/make-image.sh` -> xz -T0 -9 -k, sha256sum, packages.txt; flash + test-board PASS; release notes; publish (tag v0.5.0, gh release) ONLY on the user's word.

Division of labour (the board is reachable only when the user powers it on + Ethernet):
- The assistant runs from oleg's PC over SSH: `scripts/test-board.sh` (BOARD_PASS in mem:reference/test-board-password), NPU npu-selftest/npu-info, `tests/npu/autotune-log.sh` (~1 h log), suspend/resume via RTC alarm + SSH reconnect, Wi-Fi AP connect + iperf3 (SSID/password must be provided by the user), USB-C dmesg/state after a device is plugged, boot logo partially via the UART console (/dev/ttyUSB*, PL2303; the monitor picture itself is not visible).
- The user (physical): connect the two U.FL antennas, look at the monitor (boot logo, KDE), listen (HDMI audio, 3.5 mm), plug a USB 3 Type-C device/cable in both orientations, short header J3 (maskrom) for flashing, power-cycle the board.

Not in 0.5 (stated as not tested / later in the notes): SD boot + swap, KDE Plasma, 40-pin electrical side, USB-keyboard wake; open BL31/DRAM init and own NPU-die kernel = Later; DMC, Arm NN, Chromium = 0.6.

Related: mem:project/05-litert-migration-state, mem:project/05-prerelease-checks, mem:project/releases-roadmap, mem:feedback/release-update-rule.
