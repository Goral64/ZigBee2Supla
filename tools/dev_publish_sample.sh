#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Publishes the sample zigbee2mqtt device list and a few device states to a
# local MQTT broker, simulating zigbee2mqtt for development.
#
# Usage: tools/dev_publish_sample.sh [port] [host]

set -euo pipefail
PORT="${1:-1883}"
HOST="${2:-127.0.0.1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PUB=(mosquitto_pub -h "${HOST}" -p "${PORT}")

"${PUB[@]}" -r -t zigbee2mqtt/bridge/state -m '{"state":"online"}'
"${PUB[@]}" -r -t zigbee2mqtt/bridge/devices \
  -f "${ROOT}/tests/data/bridge_devices.json"
sleep 1
"${PUB[@]}" -t 'zigbee2mqtt/Salon/czujnik' \
  -m '{"temperature":21.4,"humidity":45.5,"pressure":1001.2,"battery":97}'
"${PUB[@]}" -t 'zigbee2mqtt/Drzwi wejsciowe' -m '{"contact":true}'
"${PUB[@]}" -t 'zigbee2mqtt/Wlacznik kuchnia' \
  -m '{"state_l1":"OFF","state_l2":"ON"}'
"${PUB[@]}" -t 'zigbee2mqtt/Lampa sypialnia' -m '{"state":"OFF"}'
"${PUB[@]}" -t 'zigbee2mqtt/Ruch korytarz' \
  -m '{"occupancy":false,"temperature":19.5}'
echo "Sample zigbee2mqtt data published to ${HOST}:${PORT}"
