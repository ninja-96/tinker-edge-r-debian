On the board (the image), Python dependencies come only from apt (e.g. `python3-numpy`), to keep the system clean.

**Why:** the user said the rule was only meant for the board "чтобы там мусор не плодить"; a Docker image on the PC (RKNN-Toolkit converter) may use pip and pinned old Python.

**How to apply:** board-side modules like `rk3399pro_npu` depend only on apt packages and Python 3.10+; PC-side tooling in Docker can pin Python 3.8 and pip packages. See mem:project/releases-roadmap.
