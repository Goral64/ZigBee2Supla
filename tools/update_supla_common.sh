#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Updates third_party/supla-common from SUPLA/supla-device and prints a
# report of protocol changes relevant to zigbee2supla.
#
# Usage: tools/update_supla_common.sh <tag|branch|commit> [repo-url]
# Example: tools/update_supla_common.sh v25.10
#
# After running it: build, run ctest and review the report.

set -euo pipefail

REF="${1:-}"
REPO="${2:-https://github.com/SUPLA/supla-device.git}"
if [[ -z "${REF}" ]]; then
  echo "Usage: $0 <tag|branch|commit> [repo-url]" >&2
  exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${ROOT}/third_party/supla-common"
FILES=(srpc.c srpc.h proto.c proto.h lck.c lck.h log.h)
# Protocol items used by zigbee2supla (keep in sync with
# tests/proto_contract_test.cpp).
USED_STRUCTS=(
  TDS_SuplaRegisterDeviceHeader TDS_SuplaDeviceChannel_E
  TDS_SuplaRegisterDevice_G TSD_SuplaRegisterDeviceResult
  TSD_SuplaRegisterDeviceResult_B TDS_SuplaDeviceChannelValue_C
  TSD_SuplaChannelNewValue TSD_SuplaChannelGroupNewValue
  TDS_SuplaChannelNewValueResult TDCS_SuplaSetActivityTimeout
  TSDC_SuplaSetActivityTimeoutResult TSD_DeviceCalCfgRequest
  TDS_DeviceCalCfgResult TSDC_SuplaVersionError TsrpcParams
  TsrpcReceivedData
)

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

echo ">> Fetching ${REPO} @ ${REF}"
git -C "${WORK}" init -q upstream
git -C "${WORK}/upstream" remote add origin "${REPO}"
git -C "${WORK}/upstream" fetch -q --depth 1 origin "${REF}"
git -C "${WORK}/upstream" checkout -q FETCH_HEAD
COMMIT="$(git -C "${WORK}/upstream" rev-parse HEAD)"
COMMIT_DATE="$(git -C "${WORK}/upstream" log -1 --format=%cs)"
SRC="${WORK}/upstream/src/supla-common"

for f in "${FILES[@]}"; do
  if [[ ! -f "${SRC}/${f}" ]]; then
    echo "!! ${f} no longer exists upstream - manual work needed" >&2
    exit 2
  fi
done

mkdir -p "${WORK}/old"
cp "${DEST}"/*.[ch] "${WORK}/old/"

proto_version() {
  sed -n 's/^#define SUPLA_PROTO_VERSION \([0-9]*\).*/\1/p' "$1"
}
OLD_VERSION="$(proto_version "${WORK}/old/proto.h")"
NEW_VERSION="$(proto_version "${SRC}/proto.h")"

for f in "${FILES[@]}"; do
  cp "${SRC}/${f}" "${DEST}/${f}"
done

cat > "${DEST}/UPSTREAM" <<EOF
repository: ${REPO}
path: src/supla-common
ref: ${REF}
commit: ${COMMIT}
commit_date: ${COMMIT_DATE}
files: ${FILES[*]}
EOF

REPORT="${ROOT}/build/supla-common-update-report.txt"
mkdir -p "${ROOT}/build"
{
  echo "supla-common update report"
  echo "=========================="
  echo "upstream: ${REPO} @ ${REF} (${COMMIT}, ${COMMIT_DATE})"
  echo
  echo "SUPLA_PROTO_VERSION: ${OLD_VERSION} -> ${NEW_VERSION}"
  echo

  echo "--- Changed files"
  for f in "${FILES[@]}"; do
    if ! cmp -s "${WORK}/old/${f}" "${DEST}/${f}"; then
      echo "  ${f}: $(diff "${WORK}/old/${f}" "${DEST}/${f}" | grep -c '^[<>]') changed line(s)"
    fi
  done
  echo

  echo "--- New local includes (may require copying more files)"
  for f in srpc.c proto.c lck.c; do
    grep -ho '#include "[^"]*"' "${DEST}/${f}" | sed 's/#include "\(.*\)"/\1/' |
      while read -r inc; do
        # Headers used only on other platforms (ESP8266 SDK, supla-server).
        case "${inc}" in espmissingincludes.h | eh.h | cfg.h) continue ;; esac
        [[ -f "${DEST}/${inc}" ]] || echo "  ${f} includes missing ${inc}"
      done
  done
  echo

  # Prints "< old" / "> new" lines of #defines matching the pattern.
  diff_defines() {
    local out
    out="$(diff <(grep -oE "$1" "${WORK}/old/proto.h" | sort) \
                <(grep -oE "$1" "${DEST}/proto.h" | sort) | grep '^[<>]' || true)"
    if [[ -n "${out}" ]]; then echo "${out}" | sed 's/^/  /'; else echo "  none"; fi
  }

  echo "--- New / removed SRPC call ids (< removed, > added)"
  diff_defines '#define SUPLA_[A-Z]*_CALL_[A-Z0-9_]* +[0-9]*'
  echo

  echo "--- New / removed channel types, functions, result codes"
  diff_defines '#define SUPLA_(CHANNELTYPE|CHANNELFNC|RESULTCODE|BIT_FUNC|CHANNEL_OFFLINE_FLAG)_[A-Z0-9_]* +[0-9xA-Fa-f]*'
  echo

  echo "--- Structures used by zigbee2supla that changed"
  extract_struct() {
    # Prints the typedef struct ending with "} <name>;".
    awk -v name="$2" '
      /typedef (struct|union)/ { buf = ""; collecting = 1 }
      collecting { buf = buf $0 "\n" }
      collecting && $0 ~ "^} *" name ";" { printf "%s", buf; collecting = 0 }
      collecting && /^} *[A-Za-z_]+;/ { collecting = 0 }
    ' "$1"
  }
  changed=0
  for s in "${USED_STRUCTS[@]}"; do
    for h in proto.h srpc.h; do
      old="$(extract_struct "${WORK}/old/${h}" "${s}")"
      new="$(extract_struct "${DEST}/${h}" "${s}")"
      if [[ -n "${old}${new}" && "${old}" != "${new}" ]]; then
        changed=1
        echo "  ${s} (${h}):"
        diff <(echo "${old}") <(echo "${new}") | sed 's/^/    /' || true
      fi
    done
  done
  [[ ${changed} == 1 ]] || echo "  none"
  echo

  echo "--- Used srpc functions with changed declarations"
  for fn in srpc_init srpc_free srpc_params_init srpc_iterate srpc_getdata \
            srpc_rd_free srpc_set_proto_version srpc_call_min_version_required \
            srpc_ds_async_registerdevice_in_chunks_g \
            srpc_ds_async_channel_value_changed_c \
            srpc_ds_async_set_channel_result srpc_ds_async_device_calcfg_result \
            srpc_dcs_async_ping_server srpc_dcs_async_set_activity_timeout; do
    old="$(grep -A3 "${fn}(" "${WORK}/old/srpc.h" | tr -s ' \n' ' ' || true)"
    new="$(grep -A3 "${fn}(" "${DEST}/srpc.h" | tr -s ' \n' ' ' || true)"
    if [[ -z "${new}" ]]; then
      echo "  ${fn}: REMOVED"
    elif [[ "${old}" != "${new}" ]]; then
      echo "  ${fn}: declaration changed"
    fi
  done
} | tee "${REPORT}"

echo
echo ">> Report saved to ${REPORT}"
echo ">> Next: build, run ctest and review the report"
