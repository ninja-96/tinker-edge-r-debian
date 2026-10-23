# Building and updating the kernel

The kernel is not taken from Debian. `scripts/build-kernel.sh` downloads the pinned
tarball from kernel.org, applies our patches and files, and builds it inside the Docker
builder. Everything that decides the result is in the repository:

| What | Where |
|---|---|
| Version, checksum, release suffix | `sources.lock` (`LINUX_VERSION`, `LINUX_SHA256`, `KERNEL_LOCALVERSION`) |
| Patches to Linux | `patches/linux/*.patch` |
| New files (mpp_service port, NPU power driver) | `kernel-mods/` |
| Kernel options | `config/linux-tinker.fragment` on top of the arm64 `defconfig` |
| Device trees | `dts/` |

## Rebuild the kernel only

```sh
./run.sh scripts/build-kernel.sh
```

Output: `out/Image`, `out/*.dtb`, `out/modules/`. The first build takes a while,
later builds are incremental.

## Move to a new Linux release (6.18.57, 6.18.58, ...)

Stable updates of the same LTS branch normally need no code changes.

1. Find the checksum of the new tarball:
   ```sh
   curl -s https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc | grep 'linux-6.18.NN.tar.xz'
   ```
2. Edit `sources.lock`: set `LINUX_VERSION=6.18.NN` and `LINUX_SHA256=<checksum>`.
3. Build: `./run.sh scripts/build-kernel.sh` (or `scripts/make-image.sh` for a full image).
   The script downloads the new tarball, checks the checksum and applies the patches.
   Patches that are already in the new release are reported as "already applied".
4. If a patch no longer applies, the build stops with the failing hunk. Rebase the patch
   on the new source (see below), keep the file name and numbering.
5. Test it (next section), then either flash a full image or update a running board.

## Move to the next LTS branch

Same steps, with more to check:

- `patches/linux/*.patch` and `kernel-mods/` (the `mpp_service` port has a compatibility
  header, `kernel-mods/drivers/soc/rockchip/mpp/mpp_compat.h`) may need updating for
  changed kernel APIs.
- Check whether our patches went upstream, then drop them: `0002` (fan53555 revision
  0xc) is a candidate, see [OPEN.md](OPEN.md).
- Options in `config/linux-tinker.fragment` can be renamed or removed. Look for
  warnings about unknown symbols in the build log, and compare `out/kernel.config` with
  the previous one.
- Rockchip device-tree bindings change between releases; `scripts/dtb-check.sh` builds
  the DTBs and reports problems.

### Rebasing a patch

```sh
tar -xf downloads/linux-6.18.NN.tar.xz -C /tmp           # or use build/linux-6.18.NN
cd /tmp/linux-6.18.NN
patch -p1 --dry-run < <repo>/patches/linux/000X-name.patch   # shows which hunk fails
# edit the file by hand, then regenerate the patch:
diff -u --label a/<path> --label b/<path> <original> <changed> > <repo>/patches/linux/000X-name.patch
```

## Install on a board that is already running

No reflashing needed:

```sh
scripts/update-kernel.sh                # or: scripts/update-kernel.sh <host or IP>
ssh debian@tinker-edge-r.local sudo reboot
```

The script copies the kernel, device trees and modules over SSH. The previous kernel
and device trees stay in `/boot/Image.old` and `/boot/dtbs.old`, and `extlinux.conf`
gets a second entry `previous`. If the new kernel does not boot, choose `previous` at the
U-Boot menu (serial console, press a key within 2 seconds), or write the image again over
USB, see [FLASHING.md](FLASHING.md). Old modules are kept in `/lib/modules/`.

Check the result: `uname -r` shows the new release, then run
`scripts/test-board.sh` ([TESTING.md](TESTING.md)).
