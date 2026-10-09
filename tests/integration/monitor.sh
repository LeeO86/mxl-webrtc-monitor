#!/usr/bin/env bash
# Integration: route a v210 + float32 flow, check HLS, TSL tally over UDP and TCP, no_signal, then waiting→running.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-webrtc-monitor}"
WRITER="${2:-$ROOT/build/mxl-flow-writer}"
MEDIAMTX="${MEDIAMTX_BIN:-/tmp/mediamtx/mediamtx}"
WORKDIR="$(mktemp -d)"
DOMAIN="$WORKDIR/mxl/srcdomain"
DOMAIN_ID="aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
VIDEO_ID="bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
AUDIO_ID="cccccccc-cccc-4ccc-8ccc-cccccccccccc"
VIDEO2_ID="dddddddd-dddd-4ddd-8ddd-dddddddddddd"
AUDIO2_ID="eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
WEB_PORT="${WEB_PORT:-18100}"
NMOS_PORT="${NMOS_PORT:-13242}"
REG_PORT="${REG_PORT:-13210}"
RTSP_PORT="${RTSP_PORT:-18554}"
API_PORT="${API_PORT:-19997}"
HLS_PORT="${HLS_PORT:-18888}"
WHEP_PORT="${WHEP_PORT:-18889}"
ICE_PORT="${ICE_PORT:-18189}"
TSL_UDP_PORT="${TSL_UDP_PORT:-18912}"
TSL_TCP_PORT="${TSL_TCP_PORT:-18913}"
PIDS=()

cleanup() {
  local status=$?
  if [[ "$status" -ne 0 && -d "$WORKDIR" ]]; then
    mkdir -p /tmp/mwm-integration-last
    cp -a "$WORKDIR"/. /tmp/mwm-integration-last/ || true
    echo "logs kept in /tmp/mwm-integration-last" >&2
  fi
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -rf "$WORKDIR"
}
trap cleanup EXIT

if [[ ! -x "$BIN" || ! -x "$WRITER" || ! -x "$MEDIAMTX" ]]; then
  echo "missing binary: monitor=$BIN writer=$WRITER mediamtx=$MEDIAMTX" >&2
  exit 1
fi

export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:/opt/mxl/lib"

python3 "$ROOT/tests/integration/fake_registry.py" "$REG_PORT" "$((REG_PORT + 1))" >"$WORKDIR/registry.log" 2>&1 &
PIDS+=($!)

mkdir -p "$DOMAIN"
"$WRITER" "$DOMAIN" "$DOMAIN_ID" "$VIDEO_ID" "$AUDIO_ID" 40 480 270 25 >"$WORKDIR/writer1.log" 2>&1 &
PIDS+=($!)

export MXL_DOMAIN_SCAN_PATH="$WORKDIR/mxl"
export MONITOR_CHANNELS=1
export HOST_ID=integration
export NMOS_SEED=integration-monitor
export NMOS_ENABLE=true
export NMOS_REGISTRY_ADDRESS=127.0.0.1
export NMOS_REGISTRY_PORT="$REG_PORT"
export NMOS_DNS_SD=false
export NMOS_PORT="$NMOS_PORT"
export WEB_PORT="$WEB_PORT"
export MEDIAMTX_RTSP_URL="rtsp://127.0.0.1:${RTSP_PORT}"
export MEDIAMTX_API_URL="http://127.0.0.1:${API_PORT}"
export MEDIAMTX_CONFIG_PATH="$WORKDIR/mediamtx.yml"
export MEDIAMTX_WHEP_PORT="$WHEP_PORT"
export MEDIAMTX_HLS_PORT="$HLS_PORT"
export MEDIAMTX_ICE_UDP_PORT="$ICE_PORT"
export ENCODER=x264
export MONITOR_PREVIEW_HEIGHT=270
export MONITOR_VIDEO_BITRATE_KBPS=800
unset MONITOR_PUBLIC_IP || true
export MONITOR_HLS_PUBLIC_URL="http://127.0.0.1:${HLS_PORT}"
export LOG_LEVEL=info
export READ_OFFSET_GRAINS=1
export STATE_DIR="$WORKDIR/state"
export SHUTDOWN_TIMEOUT_S=10
export MXL_CLEANUP_ON_EXIT=true
export TSL_ENABLE=true
export TSL_UDP_PORT TSL_TCP_PORT

mkdir -p "$WORKDIR/mxl/decoy-other"
printf '%s\n' '{"id":"11111111-1111-4111-8111-111111111111"}' >"$WORKDIR/mxl/decoy-other/domain_def.json"

"$BIN" >"$WORKDIR/monitor.log" 2>&1 &
MONITOR_PID=$!
PIDS+=("$MONITOR_PID")

for _ in $(seq 1 50); do
  if [[ -f "$WORKDIR/mediamtx.yml" ]]; then
    break
  fi
  sleep 0.1
done
"$MEDIAMTX" "$WORKDIR/mediamtx.yml" >"$WORKDIR/mediamtx.log" 2>&1 &
PIDS+=($!)

wait_http() {
  local url="$1"
  local expect="${2:-200}"
  for _ in $(seq 1 80); do
    local code
    code="$(curl -s -o /tmp/mwm-body -w '%{http_code}' "$url" || true)"
    if [[ "$code" == "$expect" ]]; then
      return 0
    fi
    sleep 0.25
  done
  echo "timeout $url (last $code)" >&2
  cat /tmp/mwm-body >&2 || true
  return 1
}

wait_http "http://127.0.0.1:${WEB_PORT}/livez"
wait_http "http://127.0.0.1:${NMOS_PORT}/x-nmos/node/v1.3/self/"

RECEIVERS="$(curl -sf "http://127.0.0.1:${NMOS_PORT}/x-nmos/node/v1.3/receivers")"
VIDEO_RX="$(python3 -c 'import json,sys; items=json.load(sys.stdin); print(next(i["id"] for i in items if i["label"].endswith("Video")))' <<<"$RECEIVERS")"
AUDIO_RX="$(python3 -c 'import json,sys; items=json.load(sys.stdin); print(next(i["id"] for i in items if i["label"].endswith("Audio")))' <<<"$RECEIVERS")"

patch() {
  local id="$1" domain="$2" flow="$3"
  curl -sf -X PATCH "http://127.0.0.1:${NMOS_PORT}/x-nmos/connection/v1.2/single/receivers/${id}/staged" \
    -H 'Content-Type: application/json' \
    -d "{\"master_enable\":true,\"activation\":{\"mode\":\"activate_immediate\"},\"transport_params\":[{\"mxl_domain_id\":\"${domain}\",\"mxl_flow_id\":\"${flow}\"}]}" >/dev/null
}

patch "$VIDEO_RX" "$DOMAIN_ID" "$VIDEO_ID"
patch "$AUDIO_RX" "$DOMAIN_ID" "$AUDIO_ID"

wait_state() {
  local want="$1" kind="${2:-video}"
  for _ in $(seq 1 80); do
    local state
    state="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/channels" | python3 -c 'import json,sys; print(json.load(sys.stdin)["channels"][0][sys.argv[1]]["state"])' "$kind")"
    if [[ "$state" == "$want" ]]; then
      echo "$kind state=$state"
      return 0
    fi
    sleep 0.25
  done
  echo "wanted $kind $want, channels:" >&2
  curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/channels" >&2 || true
  echo >&2
  tail -n 80 "$WORKDIR/monitor.log" >&2 || true
  return 1
}

wait_state running
# Audio too (MXL's continuous writer leaves lastWriteTime at 0; 1.0.3 showed no_signal).
wait_state running audio

channels_json="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/channels")"
HLS_INDEX="$(python3 -c '
import json, sys
playback = json.loads(sys.argv[1])["channels"][0]["playback"]
base = sys.argv[2]
if playback["hls"] != base + "/ch1/index.m3u8":
    raise SystemExit("hls url %s" % playback["hls"])
if playback["public"]["hls"] is not True or playback["public"]["whep"] is not False:
    raise SystemExit("public flags %s" % playback["public"])
whep = playback["whep"]
suffix = ":%s/ch1/whep" % sys.argv[3]
if not whep.startswith("http://") or not whep.endswith(suffix):
    raise SystemExit("whep url %s" % whep)
host = whep[len("http://"):-len(suffix)]
if host in ("127.0.0.1", "0.0.0.0") or any(c.isalpha() for c in host):
    raise SystemExit("whep url %s" % whep)
print(playback["hls"])
' "$channels_json" "$MONITOR_HLS_PUBLIC_URL" "$WHEP_PORT")"
echo "hls public url $HLS_INDEX"

hls_has_segments() {
  local master variant media
  master="$(curl -sfL --max-time 3 -c "$WORKDIR/cookies" -b "$WORKDIR/cookies" "$HLS_INDEX" || true)"
  printf '%s\n' "$master" >"$WORKDIR/hls-master.txt"
  variant="$(printf '%s\n' "$master" | awk '/^[^#].*\.m3u8/ { print; exit }')"
  variant="${variant%%\?*}"
  if [[ -z "$variant" ]]; then
    return 1
  fi
  if [[ "$variant" != http* ]]; then
    variant="${HLS_INDEX%/*}/${variant}"
  fi
  media="$(curl -sfL --max-time 3 -c "$WORKDIR/cookies" -b "$WORKDIR/cookies" "$variant" || true)"
  printf '%s\n' "$media" >"$WORKDIR/hls-media.txt"
  printf '%s\n' "$media" | grep -Eq 'm4s|mp4|EXTINF|EXT-X-PART'
}

for _ in $(seq 1 40); do
  if hls_has_segments; then
    echo "hls playlist has segments"
    break
  fi
  sleep 0.5
done
hls_has_segments

# The channel status shows MediaMTX's path: ready, with the H.264 track.
mtx=""
for _ in $(seq 1 20); do
  mtx="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/channels" | python3 -c 'import json,sys; m=json.load(sys.stdin)["channels"][0]["mediamtx"]; print("ok" if m["ready"] and "H264" in m["tracks"] else m)')"
  if [[ "$mtx" == "ok" ]]; then
    break
  fi
  sleep 0.5
done
if [[ "$mtx" != "ok" ]]; then
  echo "mediamtx path state: $mtx" >&2
  exit 1
fi
echo "mediamtx path ready"

# TSL 5.0, bytes from the platform's reference codec (tsl5.py). Display 0 is channel 1.
send_tsl() {
  python3 - "$@" <<'PY'
import socket, sys
proto, port, data = sys.argv[1], int(sys.argv[2]), bytes.fromhex(sys.argv[3])
if proto == "udp":
    socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(data, ("127.0.0.1", port))
else:
    with socket.create_connection(("127.0.0.1", port)) as conn:
        conn.sendall(data)
PY
}
# [tsl_lh, tsl_rh, tsl_text_tally, tally, tsl_text] of channel 1.
wait_tally() {
  local want="$1" got=""
  for _ in $(seq 1 20); do
    got="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/channels" | PYTHONIOENCODING=utf-8 python3 -c 'import json,sys; c=json.load(sys.stdin)["channels"][0]; print(json.dumps([c["tsl_lh"], c["tsl_rh"], c["tsl_text_tally"], c["tally"], c["tsl_text"]], ensure_ascii=False))')"
    if [[ "$got" == "$want" ]]; then
      echo "tally $got"
      return 0
    fi
    sleep 0.25
  done
  echo "tally: wanted $want, got $got" >&2
  return 1
}
# UDP, screen 0, UTF-16: LH red, text amber, RH green, "Kamera Zürich".
send_tsl udp "$TSL_UDP_PORT" 2400000100000000de001a004b0061006d0065007200610020005a00fc007200690063006800
wait_tally '[1, 2, 3, 3, "Kamera Zürich"]'
# TCP, DLE/STX ... DLE/ETX with a stuffed 0xFE: RH red, "þ CAM"; display 5 has no channel here.
send_tsl tcp "$TSL_TCP_PORT" fe022800000100000000c1000a00fefe002000430041004d000500ea000e00690067006e006f00720065006400fe03
wait_tally '[0, 1, 0, 1, "þ CAM"]'

# Stop the writer and expect no_signal.
kill "${PIDS[1]}" 2>/dev/null || true
wait "${PIDS[1]}" 2>/dev/null || true
unset 'PIDS[1]'
wait_state no_signal
wait_state no_signal audio

patch "$VIDEO_RX" "$DOMAIN_ID" "$VIDEO2_ID"
wait_state waiting

"$WRITER" "$DOMAIN" "$DOMAIN_ID" "$VIDEO2_ID" "$AUDIO2_ID" 25 480 270 25 >"$WORKDIR/writer2.log" 2>&1 &
PIDS+=($!)
wait_state running

metrics="$(curl -sf "http://127.0.0.1:${WEB_PORT}/metrics")"
case "$metrics" in
  *mxl_webrtc_monitor_channel_state*) ;;
  *) echo "metrics missing" >&2; exit 1 ;;
esac
page="$(curl -sf "http://127.0.0.1:${WEB_PORT}/")"
case "$page" in
  *'<div id="app">'*) ;;
  *) echo "ui missing" >&2; exit 1 ;;
esac
ready="$(curl -sf -o /dev/null -w '%{http_code}' "http://127.0.0.1:${WEB_PORT}/readyz" || true)"
if [[ "$ready" != "200" ]]; then
  echo "readyz=$ready" >&2
  curl -sS "http://127.0.0.1:${WEB_PORT}/readyz" >&2 || true
  exit 1
fi

if ! grep -q "$VIDEO2_ID" "$STATE_DIR/is05.json"; then
  echo "is05 state was not saved" >&2
  cat "$STATE_DIR/is05.json" >&2 || true
  exit 1
fi

NODE_ID="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/nmos" | python3 -c 'import json,sys; print(json.load(sys.stdin)["node_id"])')"
registered="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$((REG_PORT + 1))/x-nmos/query/v1.3/nodes/${NODE_ID}" || true)"
if [[ "$registered" != "200" ]]; then
  echo "node was not registered ($registered)" >&2
  exit 1
fi

kill -TERM "$MONITOR_PID"
set +e
wait "$MONITOR_PID"
status=$?
set -e
if [[ "$status" != "143" ]]; then
  echo "shutdown exit $status" >&2
  tail -n 80 "$WORKDIR/monitor.log" >&2 || true
  exit 1
fi

gone=""
for _ in $(seq 1 20); do
  code="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$((REG_PORT + 1))/x-nmos/query/v1.3/nodes/${NODE_ID}" || true)"
  if [[ "$code" == "404" ]]; then
    gone=1
    break
  fi
  sleep 0.25
done
if [[ -z "$gone" ]]; then
  echo "node still registered after SIGTERM" >&2
  tail -n 40 "$WORKDIR/registry.log" >&2 || true
  exit 1
fi
if [[ ! -f "$WORKDIR/mxl/decoy-other/domain_def.json" ]]; then
  echo "cleanup removed a domain this monitor does not own" >&2
  exit 1
fi
echo "integration ok"
