# Hardware video

| Function | Driver | State |
|---|---|---|
| Decode H.264/HEVC/VP9 | mainline `rkvdec`, `hantro` (V4L2 stateless) | in the kernel config (modules), not tested on the board |
| Encode H.264/VP8/JPEG | Rockchip `mpp_service` (VEPU2), ported from the BSP 6.12 (default device tree) | works: 720p H.264 at 52 frames/s with `mpi_enc_test`, about 60 with GStreamer |
| Encode H.265 | not in the RK3399 hardware | software only (x265) |
| 3D | `panfrost` (Mali T860, Midgard v5): OpenGL ES 3.1, OpenGL 3.1 | works, `eglinfo` shows Panfrost |
| Compute | Mesa Rusticl on Panfrost: OpenCL 3.0, `RUSTICL_ENABLE=panfrost` is set in `/etc/environment` | works: vector add and 256x256 matrix multiplication give correct results (about 1.7 GFLOPS) |
| Vulkan | none on the GPU: PanVK says "Mali-T860 not supported" (it covers Bifrost and newer); only the CPU driver `llvmpipe` (`mesa-vulkan-drivers`) exists | not available |

## mpp_service port

`kernel-mods/drivers/soc/rockchip/mpp/` holds the subset of the Rockchip BSP
(`develop-6.12`, `drivers/video/rockchip/mpp`) needed for the RK3399: the service,
`rkvdec`, `vdpu2`, `vepu2`. Vendor-only interfaces are provided by:

- `patches/linux/0001-iommu-rockchip-export-helpers-for-mpp.patch`: exports of
  `rockchip_iommu_{enable,disable,force_reset,mask_irq,unmask_irq,is_enabled}`;
- `mpp_compat.h`: stubs for OPP/DVFS, system monitor, PMU power-domain control and
  the SiP reset call (power domains are left to runtime PM).

The device tree variant `rk3399pro-tinker-edge-r-mpp.dts` adds the `mpp-srv`, `vepu`,
`vdpu` and `rkvdec` nodes and **disables** the mainline `vpu`/`vdec` nodes, which
use the same registers. Use one or the other.

Userspace is Rockchip's `mpp` library (https://github.com/rockchip-linux/mpp), not
included here.

## Selecting a device tree variant

The image boots the **`full`** variant by default (the `mpp` video drivers and the NPU), so hardware encoding,
the GStreamer plugins and the NPU work right after flashing. In this variant the mainline V4L2 decoders (`hantro`,
`rkvdec`) are off, because `mpp_service` owns the same hardware; for the V4L2 stateless
decoders run `sudo tinker-boot-select base` and reboot.

Variants (`dts/`): base, `-mpp`, `-npu`, `-full`. With the extlinux image the variant
is chosen by the `fdt` line of `/boot/extlinux/extlinux.conf` (all variants are in
`/boot/dtbs/`). `tinker-boot-select` belongs to the legacy Android boot image path.

## Using the hardware encoder

The encoder is only available with the `-mpp` device tree (`sudo tinker-boot-select mpp`,
reboot). The image contains Rockchip's open-source `mpp` library and its test tools
(`scripts/build-mpp.sh` builds them from the source pinned in `sources.lock`):

```sh
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -frames:v 60 -pix_fmt nv12 in.yuv
mpi_enc_test -i in.yuv -w 1280 -h 720 -f 0 -t 7 -n 60 -o out.h264 -v f   # -t 7 = H.264
mpi_dec_test -i out.h264 -t 7 -n 60 -o dec.yuv -v f
```

`/dev/mpp_service` belongs to the `video` group. `ffmpeg` and GStreamer have no mpp
plugin in Debian; to use the encoder from them, build Rockchip's `ffmpeg-rockchip` or
the GStreamer `rockchipmpp` plugin against `librockchip_mpp`.

## GStreamer

Debian's GStreamer (1.26) plus the Rockchip plugin `rockchipmpp` (built by
`scripts/build-gst-rockchip.sh` from the pinned source, LGPL-2.1) give hardware elements
on top of `/dev/mpp_service`: `mpph264enc`, `mppvp8enc`, `mppjpegenc`, `mppvideodec`,
`mppjpegdec`. Needs the `-mpp` or `-full` device tree.

```sh
gst-launch-1.0 videotestsrc num-buffers=300 ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 \
  ! mpph264enc ! h264parse ! filesink location=test.h264
gst-launch-1.0 filesrc location=test.h264 ! h264parse ! mppvideodec ! fpsdisplaysink video-sink=fakesink sync=false
```

Measured on the board: 720p H.264 encoding at about 60 frames/s (generating the test
picture on the CPU included), decoding at about 200 frames/s.
