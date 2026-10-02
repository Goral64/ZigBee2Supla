#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# zigbee2supla in Docker: setup wizard and everyday commands.
# Run ./z2s.sh without arguments; the first run starts the setup.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"
PKG_DIR=$PWD
MODE_FILE=$PKG_DIR/.z2s-mode
SERIAL_DIR=${Z2S_SERIAL_DIR:-/dev/serial/by-id}
SERVICES_OWN=(zigbee2supla)
SERVICES_Z2M=(mosquitto zigbee2mqtt)

if [ -t 1 ]; then
  GREEN=$'\e[32m' YELLOW=$'\e[33m' RED=$'\e[31m' BOLD=$'\e[1m' NC=$'\e[0m'
else
  GREEN='' YELLOW='' RED='' BOLD='' NC=''
fi

info() { echo "${GREEN}$*${NC}"; }
warn() { echo "${YELLOW}$*${NC}"; }
die() {
  echo "${RED}$*${NC}" >&2
  exit 1
}

# ask <prompt> [default] -> answer on stdout
ask() {
  local prompt=$1 default=${2-} answer
  if [ -n "$default" ]; then
    read -r -p "$prompt [$default]: " answer || true
    echo "${answer:-$default}"
  else
    while true; do
      read -r -p "$prompt: " answer || die "Przerwano."
      [ -n "$answer" ] && break
    done
    echo "$answer"
  fi
}

# ask_optional <prompt> -> answer on stdout (may be empty)
ask_optional() {
  local answer
  read -r -p "$1: " answer || true
  echo "$answer"
}

# ask_secret <prompt> -> answer on stdout (may be empty)
ask_secret() {
  local answer
  read -r -s -p "$1: " answer || true
  echo >&2
  echo "$answer"
}

# choose <prompt> <option>... -> number of the chosen option (1-based)
choose() {
  local prompt=$1 i answer
  shift
  echo "$prompt" >&2
  for ((i = 1; i <= $#; i++)); do
    echo "  $i) ${!i}" >&2
  done
  while true; do
    read -r -p "Wybór [1]: " answer || die "Przerwano."
    answer=${answer:-1}
    if [[ $answer =~ ^[0-9]+$ ]] && ((answer >= 1 && answer <= $#)); then
      echo "$answer"
      return
    fi
  done
}

yes_no() {
  local answer
  read -r -p "$1 [T/n]: " answer || true
  [[ ! $answer =~ ^[nN] ]]
}

# set_env <file> <key> <value>: sets KEY=value, replacing an existing
# (also commented out) line or appending a new one.
set_env() {
  local file=$1 key=$2 value=$3 tmp
  touch "$file"
  tmp=$(mktemp)
  KEY=$key VALUE=$value awk '
    BEGIN { done = 0 }
    !done && ($0 ~ "^#?" ENVIRON["KEY"] "=") {
      print ENVIRON["KEY"] "=" ENVIRON["VALUE"]; done = 1; next
    }
    { print }
    END { if (!done) print ENVIRON["KEY"] "=" ENVIRON["VALUE"] }
  ' "$file" >"$tmp"
  cat "$tmp" >"$file"
  rm -f "$tmp"
}

# comment_env <file> <key>: turns KEY=... into #KEY=... (option unset).
comment_env() {
  local file=$1 key=$2
  [ -f "$file" ] && sed -i "s/^$key=/#$key=/" "$file"
  return 0
}

get_env() {
  local file=$1 key=$2
  [ -f "$file" ] || return 0
  sed -n "s/^$key=//p" "$file" | tail -n 1
}

# add_compose_file <env file> <compose file>: appends to COMPOSE_FILE.
add_compose_file() {
  local file=$1 compose=$2 current
  current=$(get_env "$file" COMPOSE_FILE)
  case ":$current:" in
    *":$compose:"*) ;;
    *) set_env "$file" COMPOSE_FILE "${current:+$current:}$compose" ;;
  esac
}

# remove_compose_file <env file> <compose file>: removes from COMPOSE_FILE.
remove_compose_file() {
  local file=$1 compose=$2 current
  current=$(get_env "$file" COMPOSE_FILE)
  case ":$current:" in
    *":$compose:"*)
      current=":$current:"
      current=${current//":$compose:"/:}
      current=${current#:}
      set_env "$file" COMPOSE_FILE "${current%:}"
      ;;
  esac
}

check_docker() {
  command -v docker >/dev/null 2>&1 ||
    die "Brak Dockera. Zainstaluj go: https://docs.docker.com/engine/install/"
  docker compose version >/dev/null 2>&1 ||
    die "Brak 'docker compose' (wersja 2). Zainstaluj pakiet docker-compose-plugin."
  docker info >/dev/null 2>&1 ||
    die "Brak dostępu do Dockera. Uruchom przez sudo albo dodaj użytkownika do grupy docker."
}

load_mode() {
  MODE=standalone
  SUPLA_DOCKER_DIR=
  OWN_COORDINATOR=no
  # shellcheck source=/dev/null
  [ -f "$MODE_FILE" ] && . "$MODE_FILE"
  if [ "$MODE" = supla-docker ]; then
    COMPOSE_DIR=$SUPLA_DOCKER_DIR
  else
    COMPOSE_DIR=$PKG_DIR
  fi
}

compose() {
  (cd "$COMPOSE_DIR" && docker compose "$@")
}

our_services() {
  SERVICES=("${SERVICES_OWN[@]}")
  if [ "$OWN_COORDINATOR" = yes ]; then
    SERVICES+=("${SERVICES_Z2M[@]}")
  fi
}

host_address() {
  local ip
  ip=$(hostname -I 2>/dev/null | awk '{print $1}')
  echo "${ip:-<adres tej maszyny>}"
}

pick_adapter() {
  local devices=() d i answer
  while true; do
    devices=()
    if [ -d "$SERIAL_DIR" ]; then
      for d in "$SERIAL_DIR"/*; do
        [ -e "$d" ] && devices+=("$d")
      done
    fi
    if [ ${#devices[@]} -gt 0 ]; then
      i=$(choose "Wybierz koordynator ZigBee:" "${devices[@]}" "inna ścieżka")
      if ((i <= ${#devices[@]})); then
        echo "${devices[$((i - 1))]}"
      else
        ask "Ścieżka do koordynatora (np. /dev/ttyUSB0)"
      fi
      return
    fi
    warn "Nie widzę żadnego koordynatora w $SERIAL_DIR." >&2
    read -r -p "Podłącz go i naciśnij Enter albo wpisz ścieżkę (np. /dev/ttyUSB0): " answer ||
      die "Przerwano."
    if [ -n "$answer" ]; then
      echo "$answer"
      return
    fi
  done
}

find_supla_docker() {
  local d
  for d in "$PKG_DIR/../supla-docker" "$HOME/supla-docker" /opt/supla-docker; do
    if [ -f "$d/supla.sh" ]; then
      readlink -f "$d"
      return
    fi
  done
}

fetch_certificate() {
  local server=$1 port=$2
  command -v openssl >/dev/null 2>&1 ||
    die "Do pobrania certyfikatu serwera potrzebny jest openssl."
  mkdir -p "$PKG_DIR/certs"
  openssl s_client -connect "$server:$port" -showcerts </dev/null 2>/dev/null |
    openssl x509 -out "$PKG_DIR/certs/supla-server.crt" ||
    die "Nie udało się pobrać certyfikatu z $server:$port."
  echo "Pobrany certyfikat serwera:"
  openssl x509 -in "$PKG_DIR/certs/supla-server.crt" -noout -subject -enddate
  yes_no "Czy to Twój serwer?" ||
    die "Przerwano. Skopiuj certyfikat ręcznie do certs/supla-server.crt."
}

cmd_setup() {
  check_docker
  local where source server='' email adapter='' env_file sd_env
  local mqtt_host='' mqtt_port=1883 mqtt_user='' mqtt_pass=''

  echo "${BOLD}Konfiguracja zigbee2supla${NC}"
  echo
  where=$(choose "Gdzie działa serwer Supli?" \
    "Supla Cloud (cloud.supla.org)" \
    "supla-docker na tej maszynie" \
    "własny serwer na innej maszynie")
  case $where in
    1) server=$(ask "Adres serwera (widoczny w Supla Cloud, np. svr12.supla.org)") ;;
    2)
      SUPLA_DOCKER_DIR=$(ask "Katalog supla-docker" "$(find_supla_docker)")
      SUPLA_DOCKER_DIR=$(readlink -f "$SUPLA_DOCKER_DIR")
      [ -f "$SUPLA_DOCKER_DIR/supla.sh" ] && [ -f "$SUPLA_DOCKER_DIR/.env" ] ||
        die "W $SUPLA_DOCKER_DIR nie ma supla.sh i .env. Uruchom najpierw supla-docker."
      ;;
    3)
      server=$(ask "Adres serwera Supli (IP albo nazwa)")
      fetch_certificate "$server" 2016
      ;;
  esac
  email=$(ask "E-mail konta w Supla Cloud")

  echo
  source=$(choose "Skąd brać urządzenia ZigBee?" \
    "koordynator USB podłączony do tej maszyny (zigbee2mqtt zostanie zainstalowany)" \
    "zigbee2mqtt, który już działa (np. w Home Assistant)")
  if [ "$source" = 1 ]; then
    OWN_COORDINATOR=yes
    adapter=$(pick_adapter)
  else
    OWN_COORDINATOR=no
    mqtt_host=$(ask "Adres brokera MQTT, z którego korzysta zigbee2mqtt")
    mqtt_port=$(ask "Port brokera MQTT" 1883)
    mqtt_user=$(ask_optional "Użytkownik MQTT (Enter = bez logowania)")
    if [ -n "$mqtt_user" ]; then
      mqtt_pass=$(ask_secret "Hasło MQTT")
    fi
  fi

  if [ "$where" = 2 ]; then
    MODE=supla-docker
    sd_env=$SUPLA_DOCKER_DIR/.env
    cp supla-docker/docker-compose.zigbee2supla.yml "$SUPLA_DOCKER_DIR/"
    env_file=$SUPLA_DOCKER_DIR/zigbee2supla.env
    [ -f "$env_file" ] || cp supla-docker/zigbee2supla.env.example "$env_file"
    [ -f "$sd_env.before-zigbee2supla" ] || cp "$sd_env" "$sd_env.before-zigbee2supla"
    add_compose_file "$sd_env" docker-compose.zigbee2supla.yml
    if [ "$OWN_COORDINATOR" = yes ]; then
      cp docker-compose.zigbee2mqtt.yml "$SUPLA_DOCKER_DIR/"
      add_compose_file "$sd_env" docker-compose.zigbee2mqtt.yml
      set_env "$sd_env" ZIGBEE_ADAPTER "$adapter"
    else
      remove_compose_file "$sd_env" docker-compose.zigbee2mqtt.yml
    fi
  else
    MODE=standalone
    SUPLA_DOCKER_DIR=
    env_file=$PKG_DIR/zigbee2supla.env
    [ -f "$env_file" ] || cp zigbee2supla.env.example "$env_file"
    set_env .env COMPOSE_FILE docker-compose.yml
    if [ "$OWN_COORDINATOR" = yes ]; then
      add_compose_file .env docker-compose.zigbee2mqtt.yml
      set_env .env ZIGBEE_ADAPTER "$adapter"
    fi
    set_env "$env_file" Z2S_SUPLA_SERVER "$server"
    if [ "$where" = 1 ]; then
      set_env "$env_file" Z2S_SUPLA_SECURITY_LEVEL 0
      comment_env "$env_file" Z2S_SUPLA_CA_FILE
    else
      set_env "$env_file" Z2S_SUPLA_SECURITY_LEVEL 3
      set_env "$env_file" Z2S_SUPLA_CA_FILE /certs/supla-server.crt
    fi
  fi

  set_env "$env_file" Z2S_SUPLA_EMAIL "$email"
  set_env "$env_file" Z2S_MQTT_HOST "$mqtt_host"
  set_env "$env_file" Z2S_MQTT_PORT "$mqtt_port"
  set_env "$env_file" Z2S_MQTT_USERNAME "$mqtt_user"
  set_env "$env_file" Z2S_MQTT_PASSWORD "$mqtt_pass"
  chmod 600 "$env_file"

  cat >"$MODE_FILE" <<EOF
MODE=$MODE
SUPLA_DOCKER_DIR=$SUPLA_DOCKER_DIR
OWN_COORDINATOR=$OWN_COORDINATOR
EOF
  load_mode

  echo
  info "Zapisano konfigurację w $env_file."
  echo "Pozostałe opcje (np. include/exclude) możesz zmienić w tym pliku."
  echo
  if yes_no "Uruchomić teraz?"; then
    cmd_start
  else
    echo "Uruchomisz później poleceniem: ./z2s.sh start"
  fi
}

cmd_start() {
  check_docker
  if [ "$MODE" = supla-docker ]; then
    compose up -d
  else
    compose up -d --remove-orphans
  fi
  echo
  info "zigbee2supla działa."
  echo "Pamiętaj, żeby w Supla Cloud włączyć rejestrację nowych urządzeń."
  if [ "$OWN_COORDINATOR" = yes ]; then
    echo "zigbee2mqtt: http://$(host_address):$(get_env "$COMPOSE_DIR/.env" ZIGBEE2MQTT_PORT | grep . || echo 8080)"
    echo "Przy pierwszym uruchomieniu wybierz tam rodzaj koordynatora,"
    echo "a potem paruj urządzenia przyciskiem Permit join."
  fi
  echo "Logi: ./z2s.sh logs"
}

cmd_stop() {
  our_services
  compose stop "${SERVICES[@]}"
}

cmd_status() {
  our_services
  compose ps "${SERVICES[@]}"
  echo
  compose logs --no-log-prefix --tail 200 zigbee2supla 2>/dev/null |
    grep 'Status:' | tail -n 1 || true
}

cmd_logs() {
  local service=${1:-zigbee2supla}
  compose logs -f --tail 100 "$service"
}

cmd_update() {
  our_services
  compose pull "${SERVICES[@]}"
  compose up -d "${SERVICES[@]}"
  info "Zaktualizowano obrazy."
}

cmd_backup() {
  local data_dir name dirs=()
  if [ "$MODE" = supla-docker ]; then
    data_dir=$(get_env "$COMPOSE_DIR/.env" VOLUME_DATA)
    dirs+=("${data_dir:-./var}/zigbee2supla")
  else
    dirs+=(data)
  fi
  [ "$OWN_COORDINATOR" = yes ] && dirs+=(zigbee2mqtt)
  [ -d "$COMPOSE_DIR/certs" ] && dirs+=(certs)
  dirs+=(zigbee2supla.env)
  name=zigbee2supla-backup-$(date +%Y%m%d-%H%M%S).tar.gz
  # Files belong to container users, so tar runs in a container.
  docker run --rm --entrypoint sh \
    -v "$COMPOSE_DIR:/src:ro" -v "$PKG_DIR:/out" -w /src \
    ghcr.io/goral64/zigbee2supla:latest \
    -c "tar czf /out/$name --exclude zigbee2mqtt/log $(printf '%q ' "${dirs[@]}") && chown $(id -u):$(id -g) /out/$name && chmod 600 /out/$name"
  info "Kopia zapasowa: $PKG_DIR/$name"
  echo "Zawiera też hasła z zigbee2supla.env, przechowuj ją bezpiecznie."
}

usage() {
  cat <<EOF
Użycie: ./z2s.sh <polecenie>

  setup     konfiguracja (pytania krok po kroku)
  start     uruchomienie
  stop      zatrzymanie
  restart   ponowne uruchomienie
  status    stan kontenerów i urządzeń
  logs      logi mostka (./z2s.sh logs zigbee2mqtt – logi zigbee2mqtt)
  update    pobranie nowych wersji obrazów i ponowne uruchomienie
  backup    kopia zapasowa tożsamości urządzeń i danych zigbee2mqtt
EOF
}

main() {
  load_mode
  local cmd=${1-}
  if [ -z "$cmd" ]; then
    if [ -f "$MODE_FILE" ]; then
      cmd_status
      echo
      usage
    else
      cmd_setup
    fi
    return
  fi
  shift
  if [[ ! $cmd =~ ^(setup|help|-h|--help)$ ]] && [ ! -f "$MODE_FILE" ]; then
    die "Najpierw uruchom ./z2s.sh setup"
  fi
  case $cmd in
    setup) cmd_setup ;;
    start) cmd_start ;;
    stop) cmd_stop ;;
    restart)
      cmd_stop
      cmd_start
      ;;
    status) cmd_status ;;
    logs) cmd_logs "$@" ;;
    update) cmd_update ;;
    backup) cmd_backup ;;
    help | -h | --help) usage ;;
    *)
      usage
      exit 1
      ;;
  esac
}

main "$@"
