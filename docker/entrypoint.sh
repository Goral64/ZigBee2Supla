#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs zigbee2supla as an unprivileged user. /data is usually a bind mount
# created by Docker as root, so its ownership is fixed first.
set -e

if [ "$(id -u)" = "0" ]; then
  chown -R z2s:z2s "${Z2S_STATE_DIR:-/data}"
  exec su-exec z2s /usr/bin/zigbee2supla "$@"
fi
exec /usr/bin/zigbee2supla "$@"
