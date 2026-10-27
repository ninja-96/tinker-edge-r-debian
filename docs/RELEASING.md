# How a release is made

The order is fixed. Steps 1-2 are done automatically at the start of every major or minor release, x.0.0 or x.y.0 (standing rule of the maintainer, 2026-10-27); a patch release x.y.z (z > 0) keeps the kernel and Debian of its x.y.0; publishing (step 8) only when the maintainer says so.

| # | Step | How |
|---|---|---|
| 1 | **Update the Linux kernel and Debian to the newest versions** | `scripts/check-updates.sh --apply` writes the newest release of the long-term series in `sources.lock` (6.18.x; version and sha256 from kernel.org); `scripts/fetch-sources.sh` downloads it. Debian: the root file system is built from the live archive, so it is always rebuilt (`./run.sh scripts/build-rootfs.sh`): current point release and security updates. A new kernel series (6.19, 7.x) is a separate decision, not part of this step |
| 2 | Check that the new kernel still fits | `patches/linux/*.patch`, `kernel-mods/`, `config/linux-tinker.fragment` must apply and build (`./run.sh scripts/build-kernel.sh`). If a patch no longer applies: stop, rebase it (see [KERNEL.md](KERNEL.md)), tell the maintainer |
| 3 | Check the other pinned sources | `sources.lock` (U-Boot, TF-A, mpp, GStreamer plugins, TIM-VX, the delegate, TensorFlow): note new releases in the release notes; they are changed only on purpose |
| 4 | Links and licences | every source, library or tool the project uses has its link and licence (SPDX id and a link to the licence text) in the README section "Upstream and reference repositories", in [NOTICE.md](../NOTICE.md) and, if it ends up in the image, in `/usr/share/doc/tinker-edge-r/licenses/`; new ones are added now, the links are opened to see that they work |
| 5 | Build | `scripts/make-image.sh` (or its stages: `build-rootfs`, `build-kernel`, `build-npu-open`, `build-npu-boot`, `build-npu-host`, `build-image-mainline-uboot`), then `xz -T0 -9 -k`, `sha256sum`, `out/packages.txt` |
| 6 | Flash and test | flash the board (`scripts/flash-emmc.sh`), `scripts/test-board.sh` must give `RESULT: PASS`; skipped phases are named in the notes |
| 7 | Release notes | what is new, what works (with the counts of the test run), known problems, not supported, not tested, install, licences and the written source offer; the SHA-256 of the unpacked image |
| 8 | Publish (only on the maintainer's word) | `git push`, tag `vX.Y.Z`, `gh release create` with the image, its `.sha256` and `packages.txt`; update the About text and topics of the repository (`gh repo edit`) |
