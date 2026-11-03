When a release is made, the first thing to do is run `scripts/check-updates.sh --apply` (kernel: newest release of the series in sources.lock, e.g. 6.18.x; sha256 from kernel.org) and rebuild the Debian root file system (`./run.sh scripts/build-rootfs.sh`), without asking; then check that `patches/linux`, `kernel-mods` and the config still apply and build, and test as usual.

**Why:** the user's standing rule (2026-10-27): "делаем релиз - ты сразу проверяешь debian и linux kernel и автоматически обновляешь их до последней версии". It came with the note that 0.5 must carry 6.18.59 (0.1-0.4 shipped 6.18.56).

**Not for patch releases:** a patch release (x.y.z with z > 0, e.g. 0.4.1) keeps the kernel and Debian of its base release; the user said so for 0.4.1 (2026-10-27: "тут не надо обновлять, обновим в 0.5", "только при мажорном и минорном обновлении, не при патче").

**How to apply:** this is the first step of docs/RELEASING.md. Stay in the same long-term series (a new series such as 7.x is the user's decision, not automatic). If a kernel patch no longer applies, stop and report instead of forcing it. Publishing still needs the user's word. v0.4.0 was already built and tested with 6.18.56, so the rule starts with 0.5. See mem:project/releases-roadmap.
