The user's real aim with the NPU driver/SDK update was: put all layers on the NPU and get the speed-up. When I started building an own kernel for the NPU die with a Debian 9 / GCC 6.3 container to load a ported galcore 6.4.8, the user stopped it: "не надо ради патча драйвера так заморачиваться".

**Why:** effort must match the gain; the driver was a means, not the goal, and most gaps turned out to be in the delegate / our code or in the hardware.

**How to apply:** first measure where the limit is (hardware feature table, delegate op mapping, TIM-VX, driver compiler) and fix it in our code (server rewrites, delegate patches); propose heavy work (own kernel, driver ports) only with evidence that it is the bottleneck, and ask before starting it. See mem:project/releases-roadmap.
