#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs a command in the zigbee2supla development container (starting it if
# needed). Without arguments it opens a shell.
#
#   tools/dev/dev.sh                          # shell in the container
#   tools/dev/dev.sh cmake --preset debug
#   tools/dev/dev.sh cmake --build --preset debug
#   tools/dev/dev.sh ctest --preset debug
#   tools/dev/dev.sh --down                   # stop and remove the container
#   tools/dev/dev.sh --rebuild                # rebuild the image

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
Z2S_DIR="$(cd "${HERE}/../.." && pwd)"
Z2S_UID="$(id -u)"
Z2S_GID="$(id -g)"
export Z2S_DIR Z2S_UID Z2S_GID

compose=(docker compose -f "${HERE}/docker-compose.yml")

case "${1:-}" in
  --down)
    exec "${compose[@]}" down
    ;;
  --rebuild)
    "${compose[@]}" build --pull dev
    exec "${compose[@]}" up -d --force-recreate dev
    ;;
esac

# Quiet when the container is already running; show the output on failure.
"${compose[@]}" up -d dev > /dev/null 2>&1 || "${compose[@]}" up -d dev

tty_flag=()
[ -t 0 ] && [ -t 1 ] || tty_flag=(-T)
if [ $# -eq 0 ]; then
  set -- bash
fi
# Keep the caller's directory when it is inside the repository.
workdir="${PWD}"
case "${workdir}/" in
  "${Z2S_DIR}/"*) ;;
  *) workdir="${Z2S_DIR}" ;;
esac
exec "${compose[@]}" exec "${tty_flag[@]}" -w "${workdir}" dev "$@"
