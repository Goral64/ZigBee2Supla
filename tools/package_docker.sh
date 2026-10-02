#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the Docker package (z2s.sh + compose files) attached to releases.
#
# Usage: tools/package_docker.sh [output.tar.gz]
# Default output: build/zigbee2supla-docker.tar.gz

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(readlink -f "${1:-$ROOT/build/zigbee2supla-docker.tar.gz}")
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

PKG=$STAGE/zigbee2supla
mkdir -p "$PKG/supla-docker" "$(dirname "$OUT")"
install -m 755 "$ROOT/docker/z2s.sh" "$PKG/z2s.sh"
install -m 644 "$ROOT/docker/standalone/docker-compose.yml" \
  "$ROOT/docker/standalone/zigbee2supla.env.example" \
  "$ROOT/docker/zigbee2mqtt/docker-compose.zigbee2mqtt.yml" "$PKG/"
install -m 644 "$ROOT/docker/supla-docker/docker-compose.zigbee2supla.yml" \
  "$ROOT/docker/supla-docker/zigbee2supla.env.example" "$PKG/supla-docker/"

tar -C "$STAGE" --owner=0 --group=0 -czf "$OUT" zigbee2supla
echo "$OUT"
