#!/usr/bin/env bash
#
# HomeVideo device <-> hub end-to-end check (subsystem B against subsystem A).
#
# Starts the *real* hub (uvicorn homevideo_hub.api:create_app --factory) in a
# throwaway data directory, arms it "away", then runs the device fake backend
# once and asserts on the hub's HTTP API that the whole event chain arrived and
# stayed idempotent. Everything it creates lives in a temp dir and is removed on
# exit, success or failure.
#
# The assets it proves, one banner per assertion:
#   1. an event arrived with a non-empty snapshot and at least one segment;
#   2. the event is closed and carries a duration_ms;
#   3. re-running the device adds no duplicate row for an existing event, and
#      re-uploading the same metadata / segment is a hub idempotent no-op;
#   4. the heartbeat put the device's link online;
#   5. after PUT /arm=away the device acknowledged and the hub reports the arm
#      configuration effective.
#
# Requires the repo-root .venv (installed with `pip install -e "hub[dev]"`) and,
# for a fresh checkout, a build of build/device/hv_device.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

DEVICE_ID="v831a"
ONCE_BUDGET_MS=12000

VENV="${REPO_ROOT}/.venv"
if [[ ! -x "${VENV}/bin/uvicorn" ]]; then
  echo "ERROR: ${VENV}/bin/uvicorn not found." >&2
  echo "       Create the repo-root venv first:" >&2
  echo "         python3 -m venv .venv && .venv/bin/pip install -e 'hub[dev]'" >&2
  exit 2
fi

# The device build shells out to cmake; use the venv's copy.
export PATH="${VENV}/bin:${PATH}"
PY="${VENV}/bin/python"

BINARY="${REPO_ROOT}/build/device/hv_device"
if [[ ! -x "${BINARY}" ]]; then
  echo "==> ${BINARY} missing; configuring and building the device"
  cmake -S device -B build/device
  cmake --build build/device -j
fi
if [[ ! -x "${BINARY}" ]]; then
  echo "ERROR: build did not produce ${BINARY}" >&2
  exit 2
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/homevideo-e2e.XXXXXX")"
HUB_DATA="${TMP_DIR}/hub-data"
SPOOL="${TMP_DIR}/spool"
CONFIG="${TMP_DIR}/device.json"
HUB_LOG="${TMP_DIR}/hub.log"
mkdir -p "${HUB_DATA}" "${SPOOL}"

HUB_PID=""
cleanup() {
  if [[ -n "${HUB_PID}" ]] && kill -0 "${HUB_PID}" 2>/dev/null; then
    kill "${HUB_PID}" 2>/dev/null || true
    wait "${HUB_PID}" 2>/dev/null || true
  fi
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

FAILED=0
pass() { printf 'PASS: %s\n' "$1"; }
fail() {
  printf 'FAIL: %s\n' "$1"
  FAILED=1
  if [[ -f "${HUB_LOG}" ]]; then
    printf -- '--- hub log tail ---\n'
    tail -n 40 "${HUB_LOG}" || true
    printf -- '--------------------\n'
  fi
}

# json_get <file> <python-expr-over-`d`>  -> prints the expression's value.
json_get() {
  "${PY}" - "$1" "$2" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    d = json.load(handle)
print(eval(sys.argv[2], {"d": d}))  # noqa: S307 - fixed expressions, local script
PY
}

# check_eq <label> <file> <expr> <expected>
check_eq() {
  local label="$1" file="$2" expr="$3" expected="$4" got
  got="$(json_get "${file}" "${expr}")"
  if [[ "${got}" == "${expected}" ]]; then
    pass "${label}"
  else
    fail "${label} (expected '${expected}', got '${got}')"
  fi
}

# hub_get <curl-path> <outfile>
hub_get() {
  curl -fsS --max-time 5 "${HUB_URL}$1" -o "$2"
}

# ---------------------------------------------------------------------------
# Start the real hub
# ---------------------------------------------------------------------------

HUB_PORT="$("${PY}" - <<'PY'
import socket

sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
)"
HUB_URL="http://127.0.0.1:${HUB_PORT}"

echo "==> starting hub on ${HUB_URL} (data: ${HUB_DATA})"
HOMEVIDEO_DATA_DIR="${HUB_DATA}" \
  "${VENV}/bin/uvicorn" homevideo_hub.api:create_app --factory \
  --host 127.0.0.1 --port "${HUB_PORT}" >"${HUB_LOG}" 2>&1 &
HUB_PID=$!

READY=0
for _ in $(seq 1 100); do
  if curl -fsS --max-time 1 "${HUB_URL}/api/v1/health" >/dev/null 2>&1; then
    READY=1
    break
  fi
  if ! kill -0 "${HUB_PID}" 2>/dev/null; then
    echo "ERROR: hub exited during startup" >&2
    tail -n 40 "${HUB_LOG}" || true
    exit 1
  fi
  sleep 0.1
done
if [[ "${READY}" -ne 1 ]]; then
  echo "ERROR: hub did not answer GET /api/v1/health in time" >&2
  tail -n 40 "${HUB_LOG}" || true
  exit 1
fi

# ---------------------------------------------------------------------------
# Device configuration (small windows so one --once run yields a closed event)
# ---------------------------------------------------------------------------

cat >"${CONFIG}" <<EOF
{
  "device_id": "${DEVICE_ID}",
  "hub_url": "${HUB_URL}",
  "spool_dir": "${SPOOL}",
  "log_level": "info",
  "capture": {"width": 320, "height": 240, "fps": 15, "realtime": false},
  "encode": {"codec": "h264", "bitrate_bps": 500000, "gop_ms": 1000},
  "detect": {
    "interval_ms": 50,
    "confirm_frames": 1,
    "confirm_hold_ms": 0,
    "schedule_max_gap_ms": 1000,
    "detectors": [
      {
        "label": "person_intrusion",
        "model_path": "/data/homevideo/models/person-yolo-v1.model",
        "input_width": 128, "input_height": 128,
        "conf_threshold": 0.5, "model_version": "person-yolo-v1"
      },
      {
        "label": "fire_suspected",
        "model_path": "/data/homevideo/models/fire-yolo-v1.model",
        "input_width": 128, "input_height": 128,
        "conf_threshold": 0.5, "model_version": "fire-yolo-v1",
        "simulation": [
          {"at_run": 1, "runs": 1, "confidence": 0.93,
           "bbox": [8, 8, 64, 64], "zone": "kitchen"}
        ]
      },
      {
        "label": "smoke_suspected",
        "model_path": "/data/homevideo/models/smoke-yolo-v1.model",
        "input_width": 128, "input_height": 128,
        "conf_threshold": 0.5, "model_version": "smoke-yolo-v1"
      }
    ]
  },
  "event": {
    "pre_roll_ms": 200, "post_roll_ms": 200, "segment_ms": 1000,
    "segment_bytes": 4194304, "merge_window_ms": 1000,
    "ring_buffer_bytes": 1048576
  },
  "upload": {
    "heartbeat_ms": 1000, "arm_poll_ms": 1000, "max_backoff_ms": 2000,
    "max_attempts": 5, "rate_bytes_per_s": 10000000, "http_timeout_ms": 5000
  },
  "quota": {
    "rolling_bytes": 4194304, "pending_bytes": 16777216,
    "uploaded_bytes": 33554432, "metadata_bytes": 4194304
  },
  "health": {
    "video_stale_ms": 5000, "recording_stale_ms": 5000, "max_stage_restarts": 5
  }
}
EOF

# Fire is never gated by arm mode, so the run produces an event even if the arm
# handshake below were slow; person would require "away".
echo "==> arming the hub away"
curl -fsS --max-time 5 -X PUT "${HUB_URL}/api/v1/arm" \
  -H 'Content-Type: application/json' -d '{"mode":"away"}' >/dev/null

# ---------------------------------------------------------------------------
# Run the device once
# ---------------------------------------------------------------------------

echo "==> running device (fake) for up to ${ONCE_BUDGET_MS} ms"
if ! "${BINARY}" --config "${CONFIG}" --fake-device --once \
  --once-budget-ms "${ONCE_BUDGET_MS}"; then
  echo "ERROR: hv_device exited non-zero" >&2
  tail -n 40 "${HUB_LOG}" || true
  exit 1
fi

# ---------------------------------------------------------------------------
# A1: an event arrived with a snapshot and at least one segment
# ---------------------------------------------------------------------------

EVENTS="${TMP_DIR}/events.json"
DETAIL="${TMP_DIR}/event.json"
hub_get "/api/v1/events" "${EVENTS}"

COUNT="$(json_get "${EVENTS}" 'len(d["items"])')"
if [[ "${COUNT}" -ge 1 ]]; then
  pass "hub received at least one event (${COUNT})"
else
  fail "hub received no events"
fi

EVENT_ID="$(json_get "${EVENTS}" 'd["items"][0]["event_id"] if d["items"] else ""')"
if [[ -z "${EVENT_ID}" ]]; then
  printf '\n==> e2e summary: FAIL\n'
  exit 1
fi
hub_get "/api/v1/events/${EVENT_ID}" "${DETAIL}"

check_eq "event has a stored snapshot" "${DETAIL}" \
  'd["snapshot"] is not None and d["snapshot"]["size_bytes"] > 0' 'True'
check_eq "event has at least one segment" "${DETAIL}" \
  'len(d["segments"]) >= 1 and all(s["size_bytes"] > 0 for s in d["segments"])' 'True'

# ---------------------------------------------------------------------------
# A2: the event is closed and carries a duration
# ---------------------------------------------------------------------------

check_eq "event status is closed" "${DETAIL}" 'd["status"]' 'closed'
check_eq "event has a duration_ms" "${DETAIL}" 'd["duration_ms"] is not None' 'True'

# ---------------------------------------------------------------------------
# A4: the heartbeat marked the device link online
# ---------------------------------------------------------------------------

HEALTH="${TMP_DIR}/health.json"
hub_get "/api/v1/devices/${DEVICE_ID}/health" "${HEALTH}"
check_eq "device link is online" "${HEALTH}" 'd["link"]' 'online'

# ---------------------------------------------------------------------------
# A5: the device acknowledged the away configuration
# ---------------------------------------------------------------------------

ARM="${TMP_DIR}/arm.json"
hub_get "/api/v1/arm/status" "${ARM}"
check_eq "desired arm mode is away" "${ARM}" 'd["desired"]["mode"]' 'away'
check_eq "arm configuration is effective" "${ARM}" 'd["effective"]' 'True'
check_eq "device is listed as acknowledged" "${ARM}" \
  '"${DEVICE_ID}" in d["acked_devices"]' 'True'
check_eq "applied arm mode is away" "${ARM}" 'd["applied"] and d["applied"]["mode"]' 'away'

# ---------------------------------------------------------------------------
# A3: a second device run adds no duplicate row for the same event
#
# The device ids an event by "<device>-<boot>-<uptime>-<seq>" with a fresh
# boot id per process, so a second run legitimately opens a *new* event. The
# property under test is narrower and is what the hub actually guarantees: no
# second row for an *existing* event, and re-uploading the same bytes is a
# hub-side idempotent no-op.
# ---------------------------------------------------------------------------

FP_EXPR='((d["snapshot"] or {}).get("sha256"), [(s["seq"], s["sha256"]) for s in d["segments"]])'
before_fp="$(json_get "${DETAIL}" "${FP_EXPR}")"

echo "==> running device a second time"
if ! "${BINARY}" --config "${CONFIG}" --fake-device --once \
  --once-budget-ms "${ONCE_BUDGET_MS}"; then
  fail "second hv_device run exited non-zero"
fi

EVENTS2="${TMP_DIR}/events2.json"
DETAIL2="${TMP_DIR}/event2.json"
hub_get "/api/v1/events" "${EVENTS2}"
COUNT2="$(json_get "${EVENTS2}" 'len(d["items"])')"
hub_get "/api/v1/events/${EVENT_ID}" "${DETAIL2}"

check_eq "no duplicate event_id in the list" "${EVENTS2}" \
  'len([i["event_id"] for i in d["items"]]) == len({i["event_id"] for i in d["items"]})' 'True'
check_eq "the first event gained no duplicate row" "${DETAIL2}" "${FP_EXPR}" "${before_fp}"
if [[ "${COUNT2}" -ge "${COUNT}" ]]; then
  pass "no events lost across the second run (${COUNT} -> ${COUNT2})"
else
  fail "event count shrank across the second run (${COUNT} -> ${COUNT2})"
fi
if [[ "${COUNT2}" -gt "${COUNT}" ]]; then
  echo "INFO: second run opened $((${COUNT2} - ${COUNT})) new event(s) under a new boot id (expected)."
fi

# Directly prove hub idempotency for the existing event: resend its metadata and
# one of its segments verbatim; both must be accepted without adding rows.
"${PY}" - "${DETAIL}" "${TMP_DIR}/repost.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    d = json.load(handle)
keys = ("event_id", "device_id", "event_type", "started_at", "time_trusted",
        "device_boot_id", "device_started_ms", "model_version",
        "config_version", "arm_mode", "detections")
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump({k: d[k] for k in keys}, handle)
PY

REPOST_CODE="$(curl -sS --max-time 5 -o "${TMP_DIR}/repost_resp.json" -w '%{http_code}' \
  -X POST "${HUB_URL}/api/v1/events" -H 'Content-Type: application/json' \
  --data-binary "@${TMP_DIR}/repost.json")"
if [[ "${REPOST_CODE}" == "200" ]] && \
   [[ "$(json_get "${TMP_DIR}/repost_resp.json" 'd["created"]')" == "False" ]]; then
  pass "re-posting the same event metadata is a no-op (200, created=false)"
else
  fail "re-posting the same event metadata was not an idempotent 200 (got ${REPOST_CODE})"
fi

SEG_PATH="$(json_get "${DETAIL}" 'd["segments"][0]["path"]')"
SEG_SHA="$(json_get "${DETAIL}" 'd["segments"][0]["sha256"]')"
SEG_SEQ="$(json_get "${DETAIL}" 'd["segments"][0]["seq"]')"
SEG_CODE="$(curl -sS --max-time 5 -o /dev/null -w '%{http_code}' \
  -X PUT "${HUB_URL}/api/v1/events/${EVENT_ID}/segments/${SEG_SEQ}" \
  -H "X-Segment-Sha256: ${SEG_SHA}" --data-binary "@${SEG_PATH}")"
if [[ "${SEG_CODE}" == "200" ]]; then
  pass "re-uploading the same segment is a duplicate 200"
else
  fail "re-uploading the same segment returned ${SEG_CODE} (expected 200 duplicate)"
fi

hub_get "/api/v1/events/${EVENT_ID}" "${DETAIL2}"
check_eq "re-uploads added no segment row" "${DETAIL2}" \
  'len(d["segments"])' "$(json_get "${DETAIL}" 'len(d["segments"])')"
hub_get "/api/v1/events" "${EVENTS2}"
check_eq "re-uploads added no event row" "${EVENTS2}" 'len(d["items"])' "${COUNT2}"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

printf '\n==> e2e summary: %s\n' "$( [[ "${FAILED}" -eq 0 ]] && echo PASS || echo FAIL )"
if [[ "${FAILED}" -ne 0 ]]; then
  exit 1
fi
