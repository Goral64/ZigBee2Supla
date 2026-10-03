#!/usr/bin/with-contenv bashio
# shellcheck shell=bash
set -e

OPTIONS=/data/options.json
CONFIG=/tmp/zigbee2supla.json

# Returns an optional option, or an empty string when it is not set
# (bashio::config returns the text "null" for unset options, also when an
# empty default is given).
optional() {
  if bashio::config.has_value "$1"; then
    bashio::config "$1"
  fi
}

# Use the MQTT broker provided by Home Assistant (Mosquitto add-on) unless
# the user configured one explicitly.
MQTT_HOST="$(optional 'mqtt_host')"
if [ -n "${MQTT_HOST}" ]; then
  MQTT_PORT="$(optional 'mqtt_port')"
  MQTT_USER="$(optional 'mqtt_username')"
  MQTT_PASS="$(optional 'mqtt_password')"
elif bashio::services.available "mqtt"; then
  MQTT_HOST="$(bashio::services mqtt 'host')"
  MQTT_PORT="$(bashio::services mqtt 'port')"
  MQTT_USER="$(bashio::services mqtt 'username')"
  MQTT_PASS="$(bashio::services mqtt 'password')"
elif [ "$(bashio::config 'source')" = "zha" ]; then
  MQTT_PORT=""
  MQTT_USER=""
  MQTT_PASS=""
else
  bashio::log.warning "No MQTT broker: install the Mosquitto add-on or set mqtt_host"
  MQTT_PORT=""
  MQTT_USER=""
  MQTT_PASS=""
fi

# The add-on talks to Home Assistant through the Supervisor proxy.
jq --arg host "${MQTT_HOST:-localhost}" \
   --argjson port "${MQTT_PORT:-1883}" \
   --arg user "${MQTT_USER}" \
   --arg pass "${MQTT_PASS}" \
   --arg token "${SUPERVISOR_TOKEN:-}" \
   '. + {mqtt_host: $host, mqtt_port: $port, mqtt_username: $user,
         mqtt_password: $pass, state_dir: "/config",
         ha_websocket_url: "ws://supervisor/core/websocket",
         ha_token: $token}' \
   "${OPTIONS}" > "${CONFIG}"

if [ "$(bashio::config 'source')" = "zha" ]; then
  bashio::log.info "Starting zigbee2supla (devices from ZHA)"
else
  bashio::log.info "Starting zigbee2supla (MQTT ${MQTT_HOST}:${MQTT_PORT})"
fi
exec /usr/bin/zigbee2supla -c "${CONFIG}"
