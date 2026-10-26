# Power profiles

`tinker-power` switches the clock policy of the CPU and the GPU between three profiles. It is in
the image (`/usr/local/sbin/tinker-power`); the choice is applied **at once** when you run the command, saved in
`/etc/tinker-power.conf`, and applied again as early as possible at every boot: the CPU part by
`tinker-power.service` at the very start of the boot (before the other services), the GPU part by a udev rule
(`61-tinker-power.rules`) in the moment the GPU driver registers its devfreq device. Nothing waits for anything.

```sh
tinker-power                  # show the profile, clocks, governors and the temperature
sudo tinker-power powersave   # or: normal, performance
sudo tinker-power performance --no-save    # try it, forget it at the next reboot
```

| Profile | CPU Cortex-A53 | CPU Cortex-A72 | GPU Mali-T860 |
|---|---|---|---|
| `powersave` | `schedutil`, up to 816 MHz | `schedutil`, up to 1008 MHz | automatic, 200-400 MHz |
| `normal` (default) | `schedutil`, up to 1416 MHz | `schedutil`, up to 1800 MHz | automatic, 400-800 MHz |
| `performance` | `performance`, 1416 MHz all the time | `performance`, 1800 MHz all the time | fixed at 800 MHz |

What it does and does not do:

- It only moves the cpufreq governors and limits and the GPU devfreq limits, always inside the operating
  points of the device tree (the frequencies listed by `scaling_available_frequencies` and
  `available_frequencies`). No voltage or register is written directly.
- **The memory clock is not part of it.** DRAM runs at the clock chosen by the loader at power-on (933 MHz in
  the image) and cannot be changed while the system runs, because the DRAM frequency scaling driver (DMC) is
  switched off; it is planned for release 0.4 ([ROADMAP.md](ROADMAP.md)), after which a profile could include it.
- `performance` keeps the board warm; the kernel thermal driver still reduces the clocks if the temperature
  gets too high. `powersave` limits the speed, not the idle consumption of the board (the idle clocks are
  low in all profiles).
- A measurement on the KDE desktop showed that the load of the compositor does not depend on these clocks
  (`kwin_wayland` used the same share of a core at the highest clocks), so `performance` is not a cure for a
  slow desktop; it removes the delay of the frequency governors and costs power and heat.

`scripts/test-board.sh` has a "Power profiles" phase: it applies every profile, checks that it is recognised,
and restores the previous one.
