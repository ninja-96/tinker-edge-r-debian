"""Offline acceptance tests for board administration scripts. Run with python3."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DTB = "rk3399pro-tinker-edge-r.dtb"


class Audit(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.env = dict(os.environ, PATH=f"{self.bin}:/usr/sbin:/usr/bin:/bin")

    def command(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)

    def execute(self, expected, *args):
        run = subprocess.run(args, cwd=self.work, env=self.env, capture_output=True, text=True)
        self.assertEqual(run.returncode, expected, (run.stdout, run.stderr))
        return run.stdout

    def test_chroot_binfmt(self):
        registry = self.work / "binfmt"
        registry.mkdir()
        spec = self.work / "qemu.conf"
        spec.write_text(":qemu-aarch64:fixture")
        helper = self.work / "chroot.sh"
        helper.write_text((ROOT / "scripts/build-chroot.sh").read_text()
                          .replace("/proc/sys/fs/binfmt_misc", str(registry))
                          .replace("/usr/lib/binfmt.d/qemu-aarch64.conf", str(spec)))
        self.command("uname", 'echo "$MACHINE"')
        self.command("mountpoint", "exit 0")
        self.env["MACHINE"] = "aarch64"
        args = ["bash", "-ec", '. "$1"; ensure_binfmt', "_", helper]
        self.execute(0, *args)
        self.assertEqual(list(registry.iterdir()), [])
        self.env["MACHINE"] = "x86_64"
        (registry / "status").write_text("enabled")
        self.execute(0, *args)
        self.assertEqual((registry / "register").read_text(), spec.read_text())
        entry = registry / "qemu-aarch64"
        entry.write_text("enabled")
        self.execute(0, *args)
        self.assertEqual(entry.read_text(), "enabled")
        entry.write_text("disabled")
        (registry / "status").write_text("disabled")
        self.execute(0, *args)
        self.assertEqual(entry.read_text().strip(), "1")
        self.assertEqual((registry / "status").read_text().strip(), "1")

    def test_ssh_and_log_status(self):
        scripts = self.work / "scripts"
        scripts.mkdir()
        shutil.copy(ROOT / "scripts/test-board.sh", scripts)
        self.command("ssh", 'case "$*" in *board-selftest*) cat >/dev/null; exit "$REMOTE";; esac')
        self.command("scp", "exit 0")
        self.command("tee", 'cat >/dev/null; exit "$LOG_RC"')
        self.command("ip", 'echo "host src 127.0.0.1"')
        for remote, log, expected in [(0, 0, 0), (7, 0, 7), (0, 9, 9), (7, 9, 7)]:
            self.env.update(REMOTE=str(remote), LOG_RC=str(log))
            output = self.execute(expected, scripts / "test-board.sh", "localhost")
            self.assertIn("Board test: " + ("FAIL" if expected else "PASS"), output)

    def test_extlinux_selector(self):
        dt = self.work / "dtbs"
        dt.mkdir()
        (dt / DTB).touch()
        conf = self.work / "extlinux.conf"
        src = (ROOT / "overlay/usr/local/sbin/tinker-boot-select").read_text()
        script = self.work / "select"
        script.write_text(src.replace("DT=/boot/dtbs", f"DT={dt}").replace("CONF=/boot/extlinux/extlinux.conf", f"CONF={conf}"))
        previous = "  LABEL previous\n    FDT /boot/dtbs.old/old.dtb\n"
        active = "LABEL tinker\n  FDT /boot/dtbs/current.dtb\n"
        for original in (active + previous, previous + active):
            conf.write_text(original)
            self.execute(0, "bash", script, "base")
            self.assertIn(previous, conf.read_text())
            self.assertIn(f"fdt {dt / DTB}", conf.read_text())
            self.assertIn("current: base", self.execute(0, "bash", script))
        for original in ("LABEL tinker\n" + previous, active + active):
            conf.write_text(original)
            self.execute(1, "bash", script, "base")
            self.assertEqual(original, conf.read_text())

    def test_kernel_rollback_and_retry(self):
        boot = self.work / "boot"
        conf = boot / "extlinux/extlinux.conf"
        conf.parent.mkdir(parents=True)
        modules = self.work / "lib/modules"
        modules.mkdir(parents=True)
        src = (ROOT / "scripts/update-kernel.sh").read_text().split("<<'EOS'\n")[1].split("\nEOS\n")[0]
        src = src.replace("/boot/", f"{boot}/").replace("MODDEST=/lib/modules/", f"MODDEST={modules}/")
        installer = self.work / "install.sh"
        installer.write_text(src)
        self.env["REL"] = "test-new"
        upload = self.work / "modules/lib/modules/test-new"
        upload.mkdir(parents=True)
        (upload / "modules.order").write_text("kernel/test.ko\n")
        original = f"LABEL tinker\n  FDT /boot/dtbs/{DTB}\n  APPEND quiet\nLABEL previous-extra\n  APPEND other\n"
        conf.write_text(original)
        conf.with_suffix(".conf.prev").write_text("prior backup")
        for name in ("Image", "Image.old"):
            (boot / name).write_text(name)
        for name in ("dtbs", "dtbs.old"):
            (boot / name).mkdir()
            (boot / name / DTB).write_text(name)
        (self.work / DTB).write_text("new-dtb")
        (self.work / "Image").write_text("new")
        self.command("depmod", 'exit "${DEP_RC:-0}"')
        self.command("mv", 'case "$1" in */next/Image) [ "${FAIL_MV:-0}" = 0 ] || exit 1;; esac\nexec /bin/mv "$@"')
        for key in ("DEP_RC", "FAIL_MV"):
            self.env[key] = "1"
            self.execute(1, "sh", installer)
            del self.env[key]
            self.assertEqual(conf.read_text(), original)
            self.assertEqual(conf.with_suffix(".conf.prev").read_text(), "prior backup")
            self.assertEqual((boot / "Image.old").read_text(), "Image.old")
            self.assertEqual((boot / "dtbs" / DTB).read_text(), "dtbs")
            self.assertFalse((modules / "test-new").exists())
        alt = DTB.replace(".dtb", "-full.dtb")
        (self.work / alt).write_text("new-full")
        self.execute(0, "sh", installer)
        self.assertEqual((boot / "Image.old").read_text(), "Image")
        self.assertEqual((boot / "Image").read_text(), "new")
        self.assertEqual((boot / "dtbs.old" / DTB).read_text(), "dtbs")
        self.assertIn("LABEL previous-extra", conf.read_text())
        self.execute(1, "sh", installer)
        self.assertEqual((modules / "test-new/modules.order").read_text(), "kernel/test.ko\n")
        self.env["REL"] = "test-next"
        upload.rename(upload.with_name("test-next"))
        conf.write_text(conf.read_text().replace("/boot/dtbs/" + DTB, "/boot/dtbs/" + alt))
        (self.work / "Image").write_text("next")
        self.execute(0, "sh", installer)
        self.assertEqual((boot / "Image.old").read_text(), "new")
        self.assertEqual((boot / "dtbs.old" / alt).read_text(), "new-full")
        # The fixture translates shell paths, including the generated rollback entry.
        self.assertIn(f"dtbs.old/{alt}", conf.read_text())


    def test_flash_resume_and_compare(self):
        image = self.work / "image"
        image.write_bytes(bytes(range(251)) * 9)
        device = self.work / "device"
        device.write_bytes(bytes(512) + image.read_bytes()[:512])
        self.command("rkdt", '''case "$1" in
wl) dd if="$3" of=device bs=512 seek="$2" conv=notrunc status=none;;
rl) dd if=device of="$4" bs=512 skip="$2" count="$3" status=none;;
esac''')
        command = f'source "{ROOT}/scripts/rockchip-usb.sh"; die() {{ exit 1; }}; SUDO= RKDT=rkdt; write_and_verify image 1 1 2 0'
        self.execute(0, "bash", "-ec", command)
        with device.open("r+b") as f:
            f.seek(512)
            f.write(b"x")
        self.execute(1, "bash", "-ec", command)
        # Only the whole-image path allows the vendor-storage difference in MiB 3.
        image.write_bytes(bytes(4 * 1048576 + 1))
        device.write_bytes(image.read_bytes())
        with device.open("r+b") as f:
            f.seek(3 * 1048576)
            f.write(b"x")
        resume = command.replace("image 1 1 2 0", "image 0 8193 2048")
        self.execute(0, "bash", "-ec", resume + " 1")
        self.execute(1, "bash", "-ec", resume + " 0")
        with device.open("r+b") as f:
            f.truncate(3 * 1048576)
        self.execute(1, "bash", "-ec", resume + " 1")
        self.command("sleep", "exit 0")
        for failure in ("exit 1", "echo Failed"):
            self.command("rkdt", 'echo attempt >> calls; ' + failure)
            self.execute(1, "bash", "-ec", command)
            calls = self.work / "calls"
            self.assertEqual(calls.read_text().splitlines(), ["attempt"] * 3)
            calls.unlink()

    def test_image_audit(self):
        scripts = self.work / "scripts"
        scripts.mkdir()
        shutil.copy(ROOT / "scripts/audit-free.sh", scripts)
        (self.work / "out").mkdir()
        loader = b"Tinker" * 50
        (self.work / "out/u-boot-rockchip.bin").write_bytes(loader)
        tree = self.work / "root"
        apt = tree / "etc/apt/sources.list.d"
        apt.mkdir(parents=True)
        status = tree / "var/lib/dpkg/status"
        status.parent.mkdir(parents=True)
        main = "deb https://example.invalid trixie main\n"
        (apt.parent / "sources.list").write_text(main)
        for raw, source, code in [(b"Package: one\n", "Enabled: no  \nComponents: contrib", 0),
                                  (b"Package: one\n", "Components: main\n contrib", 1),
                                  (b"Package: one\n", "Components: main\x00", 2),
                                  (b"Package: one\n\x00partial", "", 2)]:
            status.write_bytes(raw)
            (apt / "custom.sources").write_text("Types: deb\nURIs: https://example.invalid\nSuites: trixie\n" + source)
            (apt / "extra.list").write_text(main + "# deb https://example.invalid trixie non-free\n")
            self.execute(0, "mke2fs", "-q", "-F", "-t", "ext4", "-d", tree, "rootfs", "4M")
            with (self.work / "image").open("wb") as img:
                img.seek(64 * 512)
                img.write(loader)
                img.seek(128 * 512)
                img.write((self.work / "rootfs").read_bytes())
            self.env["ROOT_START"] = "128"
            output = self.execute(code, scripts / "audit-free.sh", "image")
            if code == 2:
                self.assertNotIn("RESULT:", output)
            else:
                self.assertIn("RESULT: " + ("NOT free" if code else "free"), output)

    def test_sd_image_identity(self):
        scripts = self.work / "scripts"
        scripts.mkdir()
        shutil.copy(ROOT / "scripts/build-image-mainline-uboot.sh", scripts)
        (self.work / "sources.lock").write_text("LINUX_VERSION=6.18\nKERNEL_LOCALVERSION=\n")
        stage = scripts / "stage-rootfs.sh"
        stage.write_text('#!/bin/sh\nmkdir -p "$1/etc"\n'
                         'echo "LABEL=rootfs / ext4 defaults,noatime 0 1" > "$1/etc/fstab"\n')
        stage.chmod(0o755)
        out = self.work / "out"
        out.mkdir()
        for name in ("u-boot-rockchip-ddrblob.bin", "Image", "rootfs.tar", "rk3399pro-tinker-edge-r-browser.dtb"):
            (out / name).write_bytes(b"fixture")
        self.env.update(ROOTFS_MIB="4", FREE="0", DTB_VARIANT="browser")
        ids = []
        for sd, suffix, label in (("0", "", "rootfs"), ("1", "-sd", "rootfs-sd")):
            self.env["SD"] = sd
            self.execute(0, scripts / "build-image-mainline-uboot.sh")
            image = out / f"tinker-edge-r-debian13{suffix}.img"
            part = json.loads(self.execute(0, "sfdisk", "--json", image))["partitiontable"]["partitions"][0]
            uid = part["uuid"].lower()
            ids.append(uid)
            with image.open("rb") as f:
                f.seek(64*512)
                self.assertEqual(f.read(7), b"fixture")
            fs = out / f"rootfs{suffix}.ext4"
            self.assertEqual(self.execute(0, "blkid", "-p", "-s", "LABEL", "-o", "value", fs).strip(), label)
            self.assertIn(f"root=PARTUUID={uid} ", self.execute(0, "debugfs", "-R", "cat /boot/extlinux/extlinux.conf", fs))
            self.assertIn(f"LABEL={label} / ", self.execute(0, "debugfs", "-R", "cat /etc/fstab", fs))
            if sd == "1":
                info = self.execute(0, "dumpe2fs", "-h", fs)
                self.assertRegex(info, r"(?m)^Reserved block count:\s+0$")
        self.assertNotEqual(*ids)
        loader_only = out / "tinker-edge-r-uboot-emmc.img"
        self.assertEqual(loader_only.stat().st_size, 16 * 1024 * 1024)
        with loader_only.open("rb") as f:
            self.assertEqual(f.read(64*512), bytes(64*512))
            self.assertEqual(f.read(7), b"fixture")
        # The first-boot helper grows the final partition on cards of any size.
        for card_gb in (8, 32, 64):
            with image.open("r+b") as f:
                f.truncate(card_gb * 10**9)
            result = subprocess.run(["sfdisk", "--no-reread", "-q", "-N", "1", image],
                                    input=", +\n", env=self.env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            table = json.loads(self.execute(0, "sfdisk", "--json", image))["partitiontable"]
            self.assertEqual(table["partitions"][0]["start"] + table["partitions"][0]["size"] - 1,
                             table["lastlba"])

    def test_loader_only_write_ranges(self):
        scripts = self.work / "scripts"
        scripts.mkdir()
        shutil.copy(ROOT / "scripts/flash-emmc-loader.sh", scripts)
        (scripts / "rockchip-usb.sh").write_text(
            'check_usb() { :; }\nload_loader() { :; }\n'
            'write_and_verify() { echo "$2 $(stat -c %s "$1")" >> calls; }\n')
        tools = self.work / "out/tools"
        tools.mkdir(parents=True)
        rkdt = tools / "rkdeveloptool"
        rkdt.write_text('#!/bin/sh\ncase "$1" in ld) echo DevNo=1;; rfi) echo "Flash Sectors: 65536";; esac\n')
        rkdt.chmod(0o755)
        (tools / "rk3399_loader.bin").touch()
        self.command("sudo", 'exec "$@"')
        loader = self.work / "loader.img"
        size = 32768 * 512
        with loader.open("wb") as f:
            f.seek(16384 * 512)
            f.write(bytes.fromhex("d00dfeed"))
            f.truncate(size)
        backup = self.work / "backup.img"
        args = [str(scripts / "flash-emmc-loader.sh"), "--backup", str(backup), str(loader)]
        for backup_size, confirmation, expected in ((512, "LOADER-ONLY\n", 1),
                                                   (65536 * 512, "NO\n", 1),
                                                   (65536 * 512, "LOADER-ONLY\n", 0)):
            with backup.open("wb") as f:
                f.truncate(backup_size)
            (self.work / "backup.img.sha256").write_text(self.execute(0, "sha256sum", backup))
            result = subprocess.run(args, cwd=self.work, env=self.env, input=confirmation,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, expected, (result.stdout, result.stderr))
            if expected:
                self.assertFalse((self.work / "calls").exists())
        self.assertEqual((self.work / "calls").read_text().splitlines(),
                         [f"64 {size-64*512}", f"{65536-33} {33*512}", f"0 {64*512}"])



if __name__ == "__main__":
    unittest.main()
