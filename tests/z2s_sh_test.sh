#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests of docker/z2s.sh: the setup wizard is fed answers on stdin and
# docker is replaced by a stub that records its calls.

# Literal $ in single quotes is intended (passwords with special characters).
# shellcheck disable=SC2016
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILURES=0

fail() {
  echo "FAIL: $*" >&2
  FAILURES=$((FAILURES + 1))
}

expect_line() {
  local file=$1 line=$2
  grep -qxF -- "$line" "$file" || fail "$file: missing line '$line'"
}

expect_no_match() {
  local file=$1 pattern=$2
  if grep -q -- "$pattern" "$file"; then
    fail "$file: unexpected '$pattern'"
  fi
}

# Stub docker: setup checks pass, every other call is logged.
mkdir -p "$WORK/bin"
cat >"$WORK/bin/docker" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  "compose version" | info) exit 0 ;;
esac
echo "$PWD|$*" >>"$DOCKER_LOG"
EOF
chmod +x "$WORK/bin/docker"
export PATH=$WORK/bin:$PATH
export DOCKER_LOG=$WORK/docker.log

mkdir -p "$WORK/serial"
touch "$WORK/serial/usb-ITead_Sonoff_Zigbee_3.0_USB_Dongle_Plus-if00"
export Z2S_SERIAL_DIR=$WORK/serial

# new_package <name>: unpacks a fresh package, prints its directory.
new_package() {
  mkdir -p "$WORK/$1"
  "$ROOT/tools/package_docker.sh" "$WORK/$1.tar.gz" >/dev/null
  tar -C "$WORK/$1" -xzf "$WORK/$1.tar.gz"
  echo "$WORK/$1/zigbee2supla"
}

# --- Supla Cloud + own coordinator -----------------------------------------
pkg=$(new_package cloud)
printf '%s\n' 1 svr12.supla.org user@example.com 1 1 n |
  "$pkg/z2s.sh" setup >/dev/null 2>&1
env=$pkg/zigbee2supla.env
expect_line "$env" "Z2S_SUPLA_SERVER=svr12.supla.org"
expect_line "$env" "Z2S_SUPLA_EMAIL=user@example.com"
expect_line "$env" "Z2S_SUPLA_SECURITY_LEVEL=0"
expect_line "$env" "Z2S_MQTT_HOST="
expect_no_match "$env" "^Z2S_SUPLA_CA_FILE="
expect_line "$pkg/.env" \
  "COMPOSE_FILE=docker-compose.yml:docker-compose.zigbee2mqtt.yml:docker-compose.zigbee2mqtt-usb.yml"
expect_no_match "$pkg/.env" "^ZIGBEE2MQTT_SERIAL_PORT="
expect_line "$pkg/.env" \
  "ZIGBEE_ADAPTER=$WORK/serial/usb-ITead_Sonoff_Zigbee_3.0_USB_Dongle_Plus-if00"
expect_line "$pkg/.z2s-mode" "OWN_COORDINATOR=yes"
[ "$(stat -c %a "$env")" = 600 ] || fail "$env: not mode 600"

: >"$DOCKER_LOG"
"$pkg/z2s.sh" start >/dev/null
expect_line "$DOCKER_LOG" "$pkg|compose up -d --remove-orphans"
: >"$DOCKER_LOG"
"$pkg/z2s.sh" stop >/dev/null
expect_line "$DOCKER_LOG" "$pkg|compose stop zigbee2supla mosquitto zigbee2mqtt"

# --- Supla Cloud + existing zigbee2mqtt, password with special characters --
pkg=$(new_package mqtt)
printf '%s\n' 1 svr3.supla.org u@example.com 3 192.168.1.10 1883 \
  mqttuser 'p&a/s$s\w' n | "$pkg/z2s.sh" setup >/dev/null 2>&1
env=$pkg/zigbee2supla.env
expect_line "$env" "Z2S_MQTT_HOST=192.168.1.10"
expect_line "$env" "Z2S_MQTT_USERNAME=mqttuser"
expect_line "$env" 'Z2S_MQTT_PASSWORD=p&a/s$s\w'
expect_line "$pkg/.env" "COMPOSE_FILE=docker-compose.yml"
expect_no_match "$pkg/.env" "ZIGBEE_ADAPTER"
: >"$DOCKER_LOG"
"$pkg/z2s.sh" stop >/dev/null
expect_line "$DOCKER_LOG" "$pkg|compose stop zigbee2supla"

# --- supla-docker on this machine ------------------------------------------
sd=$WORK/supla-docker
mkdir -p "$sd"
touch "$sd/supla.sh"
printf '%s\n' "VOLUME_DATA=./var" \
  "COMPOSE_FILE=docker-compose.yml:docker-compose.standalone.yml" >"$sd/.env"
pkg=$(new_package sd)
printf '%s\n' 2 "$sd" admin@example.com 1 1 n |
  "$pkg/z2s.sh" setup >/dev/null 2>&1
expect_line "$sd/.env" "COMPOSE_FILE=docker-compose.yml:docker-compose.standalone.yml:docker-compose.zigbee2supla.yml:docker-compose.zigbee2mqtt.yml:docker-compose.zigbee2mqtt-usb.yml"
expect_line "$sd/.env.before-zigbee2supla" \
  "COMPOSE_FILE=docker-compose.yml:docker-compose.standalone.yml"
expect_line "$sd/zigbee2supla.env" "Z2S_SUPLA_EMAIL=admin@example.com"
[ -f "$sd/docker-compose.zigbee2supla.yml" ] || fail "compose file not copied"
[ -f "$sd/docker-compose.zigbee2mqtt.yml" ] || fail "zigbee2mqtt not copied"
[ -f "$sd/docker-compose.zigbee2mqtt-usb.yml" ] || fail "zigbee2mqtt-usb not copied"
[ -f "$sd/docker-compose.zigbee2mqtt-network.yml" ] ||
  fail "zigbee2mqtt-network not copied"
: >"$DOCKER_LOG"
"$pkg/z2s.sh" update >/dev/null
expect_line "$DOCKER_LOG" "$sd|compose pull zigbee2supla mosquitto zigbee2mqtt"
expect_line "$DOCKER_LOG" "$sd|compose up -d zigbee2supla mosquitto zigbee2mqtt"

# Setup again with a network coordinator: no USB device any more.
printf '%s\n' 2 "$sd" admin@example.com 2 192.168.1.50 '' 1 n |
  "$pkg/z2s.sh" setup >/dev/null 2>&1
expect_line "$sd/.env" "COMPOSE_FILE=docker-compose.yml:docker-compose.standalone.yml:docker-compose.zigbee2supla.yml:docker-compose.zigbee2mqtt.yml:docker-compose.zigbee2mqtt-network.yml"
expect_line "$sd/.env" "ZIGBEE2MQTT_SERIAL_PORT=tcp://192.168.1.50:6638"
expect_line "$sd/.env" "ZIGBEE2MQTT_SERIAL_ADAPTER=zstack"
expect_no_match "$sd/.env" "^ZIGBEE_ADAPTER="

# Setup again without own coordinator: zigbee2mqtt leaves COMPOSE_FILE,
# zigbee2supla is not added twice.
printf '%s\n' 2 "$sd" admin@example.com 3 mosquitto.lan 1883 '' n |
  "$pkg/z2s.sh" setup >/dev/null 2>&1
expect_line "$sd/.env" "COMPOSE_FILE=docker-compose.yml:docker-compose.standalone.yml:docker-compose.zigbee2supla.yml"
expect_line "$sd/zigbee2supla.env" "Z2S_MQTT_HOST=mosquitto.lan"
expect_line "$sd/zigbee2supla.env" "Z2S_MQTT_USERNAME="

# --- Supla Cloud + network coordinator on another port --------------------
pkg=$(new_package net)
printf '%s\n' 1 svr12.supla.org user@example.com 2 10.0.0.7 6653 2 n |
  "$pkg/z2s.sh" setup >/dev/null 2>&1
expect_line "$pkg/.env" \
  "COMPOSE_FILE=docker-compose.yml:docker-compose.zigbee2mqtt.yml:docker-compose.zigbee2mqtt-network.yml"
expect_line "$pkg/.env" "ZIGBEE2MQTT_SERIAL_PORT=tcp://10.0.0.7:6653"
expect_line "$pkg/.env" "ZIGBEE2MQTT_SERIAL_ADAPTER=ember"
expect_no_match "$pkg/.env" "ZIGBEE_ADAPTER"
expect_line "$pkg/.z2s-mode" "OWN_COORDINATOR=yes"

# --- commands before setup -------------------------------------------------
pkg=$(new_package fresh)
if "$pkg/z2s.sh" start >/dev/null 2>&1; then
  fail "start before setup should fail"
fi

if [ "$FAILURES" -gt 0 ]; then
  echo "$FAILURES failure(s)" >&2
  exit 1
fi
echo "z2s.sh: all tests passed"
