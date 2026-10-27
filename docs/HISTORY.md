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

## NPU: TensorFlow Lite 2.21 (release 0.4.1)

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | TIM-VX newer than v1.1.32 crashes on the first graph: `query_hardware_caps: CHECK STATUS(-10)` | not the driver: the SDK headers of newer TIM-VX tags enlarge `vx_hardware_caps_params_ext_t`, and the `libOpenVX` 6.4.6 of the firmware refuses the size | build any TIM-VX with the 6.4.6 headers of v1.1.32: all its old unit tests pass |
| 2 | int8 models with a float input or output (`QUANTIZE` / `DEQUANTIZE`, `ai-edge-torch`) crash in `DataConvert::BindOutput` | our delegate patch tested `tensors_[i].get() == nullptr`: on the `main` branch `tensors_` is a `std::map`, so `[]` inserted an empty entry and the output tensor was never created | look it up with `find` |
| 3 | Keras MobileNet V2 int8: "the first run failed" (`RESHAPE`: 1000 != 1000000) | the new delegate leaves a `RESHAPE` with a computed shape (`SHAPE -> STRIDED_SLICE -> PACK`) to the CPU, between two NPU parts, and the shape it gets from the NPU part is broken | the server makes such a shape a constant (the output shape is known) and removes the dead chain |
| 4 | Comparisons, logical operators, `CAST` crash the engine (segmentation fault) | the `main` delegate takes them; TIM-VX crashes on bool tensors with driver 6.4.6 | the delegate never gives them to the NPU |
| 5 | `autotune()` of EfficientNet-B0: a candidate crashes while compiling | the model and the candidate together (about 550 MB each plus the compiler) did not fit into the 2 GB of the NPU die | autotune unloads its own copy first |
| 6 | A heavy float model is killed after 30 s | the server waited 30 s for a run, whatever the host allowed | 300 s |
| 7 | A small model takes 3 ms per `exec` through the daemon, 1.8 ms directly | the daemon and the client in Python: the hand-over between threads under the GIL costs about 1 ms | daemon and library in C++ (1.5 ms through the daemon) |

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
- Under `set -e`, `[ -n "$x" ] && cmd` as the last command of a loop body makes the loop fail on the last (empty)
  line: the script stops silently. Use `if`.
- A partial clone (`--filter`) cannot serve its commits to another local repository: fetch from the remote.
