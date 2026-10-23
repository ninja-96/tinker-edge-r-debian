# Bring-up log: what went wrong and why

The sequence of problems on the way from "no sign of life" to a booting board, in
the order they were found. Useful as a checklist when something similar happens.

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | Image written, board silent | the eMMC write was cut at **32 MiB**: the RK3399Pro USB loader silently drops writes beyond that, and every piece reported success | read back and compare (`scripts/verify-emmc.sh`); use the **RK3399** loader |
| 2 | No console output at all | the ASUS U-Boot prints on UART2 = microSD socket pads; the kernel console was set to UART2 too | console on **UART0**, header pins 8/10 |
| 3 | Still silent with a complete image and the ASUS U-Boot | not established. Hypothesis: the ASUS U-Boot runs the raw `Image` at 2 MiB + 0x80000 but a mainline kernel needs a 2 MiB boundary; a trampoline did not help | dropped the ASUS bootloader |
| 4 | `rkdeveloptool db` hung | a second `db` into an already running loader | detect the running loader with `rfi` and skip `db` |
| 5 | Verification "failed" at the last MiB | the comparison read a full MiB for the shorter last piece of the image | compare only the bytes of the image |
| 6 | Mainline U-Boot stopped after `U-Boot TPL 2026.10` | open LPDDR4 init with the Rock Pi 4 timings does not suit this memory | DRAM init blob from Rockchip in front of the open SPL |
| 7 | `vop` video probe errors, "bad CRC, using default environment" | no display set up in U-Boot, empty environment | harmless |
| 8 | Boots, but no SSH at first | the board had to finish its first boot (root filesystem growth, SSH host keys) | wait 1-2 minutes |

## Tooling lessons

- `pkill -f <script name>` inside a command that contains the same text kills the
  command's own shell; kill by PID.
- Editing a bash script while it runs corrupts the run (bash reads it lazily).
- `rkdeveloptool ld` prints "Maskrom" for a board that already runs the downloaded
  loader.
- A changing `/dev/ttyUSB0` permission after re-plugging needs a udev rule, not
  `chmod`.
- Kernel release strings: any file named `localversion*` in the object directory is
  appended to the kernel release.
- TF-A with binutils 2.44: long-call veneers inflate the PMU SRAM region; avoid
  direct branches there (see [BOOT.md](BOOT.md)).
