# Testing a board

`scripts/test-board.sh` runs a stress and health test on a board over SSH and prints a
PASS/FAIL table. Run it after every new image or kernel.

```sh
sudo apt install iperf3          # on the PC, for the network phase
scripts/test-board.sh            # 60 s per load phase, about 10 minutes in total
scripts/test-board.sh -d 300 192.168.1.53    # longer soak, board given by IP
```

The SSH password is asked once. The test needs a working network on the board: the first
run installs `stress-ng`, `iperf3`, `glmark2-es2-drm` and a few small tools with `apt`.
The log is written to `out/test-<date>.log`. For a meaningful network result the PC
should be on wired gigabit Ethernet; over Wi-Fi the PC is the bottleneck.

Before running, plug in what you want checked: a USB 3 drive in each USB-A port (and the
USB-C port with an adapter), a monitor on HDMI, headphones or speakers on the 3.5 mm jack.

## What is tested

| Phase | Method | FAIL means |
|---|---|---|
| Static checks | failed systemd units, `/dev/dri/renderD128`, cpufreq, Wi-Fi interface, `hci0` | a driver did not probe |
| USB | xHCI controllers, attached devices with link speed, read 256 MiB from a USB disk | controller missing, disk unreadable |
| Display | DRM card, HDMI connector and monitor, DisplayPort connector | no DRM card |
| Audio | every playback card gets a 440 Hz tone, capture for 2 s | `speaker-test` or `arecord` error |
| CPU | `stress-ng --cpu` on all 6 cores, then on the Cortex-A53 and Cortex-A72 clusters separately, results verified | verification error |
| RAM | `stress-ng --vm` on 70% of memory with verification | data mismatch, OOM |
| eMMC | write 512 MiB, drop caches, read back, compare SHA-256; write must be 10 MiB/s or more, read 20 MiB/s or more, no new data CRC errors | checksum mismatch, slow bus, CRC errors |
| GPU | `eglinfo` must show Panfrost, `glmark2-es2-drm --off-screen` | software rendering, crash |
| Hardware video | with the `-mpp` device tree: H.264 encode and decode of a generated 720p clip with `mpi_enc_test` / `mpi_dec_test`, frame count checked with `ffprobe`; otherwise the V4L2 decoders are listed | no output, wrong frame count |
| 40-pin header | every plain GPIO pin driven high/low and read, I2C6/I2C7 scan, `/dev/spidev1.0` and `spidev5.0`, three PWM channels; with `GPIO_LOOPBACK="7:29 31:37"` and jumper wires also the electrical loopback | line busy, device missing |
| NPU | `npu-selftest` as the ordinary user (group `plugdev`, no root, through `npu-daemon`): the chip identity, two small TensorFlow Lite models (int8 per-channel, uint8) against the result of TFLite on the CPU, speed, the compiled-graph cache, two models at once, attaching to a loaded model (also from a second process), stopping one model, a stopped model refuses to run, a broken model is refused and the NPU stays usable (17 checks); needs the `-npu` or `-full` device tree | wrong result, NPU not found, a broken model takes the NPU down, the daemon does not answer |
| RTC | written from the system clock and read back | differs by more than 5 s |
| MIPI | reports camera and DSI display if they are described in the device tree | (SKIP until described) |
| Network | `iperf3` upload and download to the PC | iperf3 error |
| Wi-Fi, Bluetooth | `iw scan`, `bluetoothctl scan` | no access points seen |
| Soak | CPU + RAM + GPU + network at the same time | any error under combined load |
| Health | temperature below 85 C, no oops / RCU stall / mmc / regulator / panfrost errors in `dmesg` | suspicious kernel messages |

A phase is reported as SKIP when its hardware is missing (no monitor, no USB disk, no
network server). SKIP is not a pass: attach the device and run again.

## What a script cannot check

Whether sound is audible, whether a monitor shows a picture and whether both USB-C
orientations work are judged by a person. The test only proves that the drivers run
without errors. Please check these by hand and note the result in
[ROADMAP.md](ROADMAP.md).
