#!/usr/bin/env bash
# Run a command inside the build container with the project mounted at /work.
#   ./run.sh                 interactive shell
#   ./run.sh scripts/foo.sh  run a script
set -euo pipefail
cd "$(dirname "$0")"
IMAGE=tinker-edge-r-builder:trixie

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build -t "$IMAGE" docker
fi

# --privileged: rootfs creation needs mount/chroot/mknod inside the container.
TTY=(); [ -t 0 ] && TTY=(-it)
exec docker run --rm "${TTY[@]}" --privileged \
    -v "$PWD":/work -w /work \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$IMAGE" "${@:-bash}"
