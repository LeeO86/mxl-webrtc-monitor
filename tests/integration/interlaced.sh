#!/usr/bin/env bash
# Integration (1.4.1): interlaced inputs. MXL 218ddaa stores an interlaced flow as one field per grain
# (height / 2 lines at twice the definition's grain_rate); 1.4.0 took each field for a frame (squashed
# into the upper half, pushed at twice the frame rate, so the preview stalled). Four channels:
#   ch1 a 480x272 i25 tff flow, ch2 the same bff, ch3 a tff flow whose flow_def.json was rewritten to
#   say progressive (a grain smaller than the definition says), ch4 a progressive flow whose
#   flow_def.json was rewritten to say interlaced_tff (whole frames, not fields: format_mismatch), ch5 a
#   tff flow in a domain that keeps only 3 fields (60 ms): READ_OFFSET_GRAINS=2 frames is always too late.
# Checks: ch1 and ch2 run at the field rate (grains) and the frame rate (encoder, stream timestamps),
# the decoded preview is the first field over the whole picture (preview height <= a field), then the
# woven frame (preview height = the frame); ch3 is `unsupported` (grain_too_small), counted and logged
# once, ch4 is `unsupported` (format_mismatch), ch5 is `no_signal` with a published slate and waits a
# grain per too-late read (1.4.0 retried at once: one core busy, the channel left in `waiting`), the
# process stays up, and ch3 recovers when its writer re-creates the flow.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-webrtc-monitor}"
WRITER="${2:-$ROOT/build/mxl-flow-writer}"
MEDIAMTX="${MEDIAMTX_BIN:-/tmp/mediamtx/mediamtx}"
WORKDIR="$(mktemp -d)"
DOMAIN="$WORKDIR/mxl/srcdomain"
DOMAIN_ID="aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
TFF="10000000-0000-4000-8000-000000000001"
BFF="10000000-0000-4000-8000-000000000002"
LIE="10000000-0000-4000-8000-000000000003"
FRAMES="10000000-0000-4000-8000-000000000005"
SHORT_DOMAIN="$WORKDIR/mxl/short"
SHORT_DOMAIN_ID="bbbbbbbb-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
SHORT="10000000-0000-4000-8000-000000000006"
WEB_PORT="${WEB_PORT:-18200}"
NMOS_PORT="${NMOS_PORT:-13252}"
REG_PORT="${REG_PORT:-13220}"
RTSP_PORT="${RTSP_PORT:-18564}"
API_PORT="${API_PORT:-19987}"
HLS_PORT="${HLS_PORT:-18898}"
WHEP_PORT="${WHEP_PORT:-18899}"
ICE_PORT="${ICE_PORT:-18199}"
WIDTH=480
HEIGHT=272
PIDS=()

cleanup() {
  local status=$?
  if [[ "$status" -ne 0 && -d "$WORKDIR" ]]; then
    mkdir -p /tmp/mwm-interlaced-last
    cp -a "$WORKDIR"/. /tmp/mwm-interlaced-last/ || true
    echo "logs kept in /tmp/mwm-interlaced-last" >&2
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
command -v gst-launch-1.0 >/dev/null || { echo "gst-launch-1.0 is needed to decode the preview" >&2; exit 1; }

export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:/opt/mxl/lib"
export PATH="$(dirname "$MEDIAMTX"):$PATH"

python3 "$ROOT/tests/integration/fake_registry.py" "$REG_PORT" "$((REG_PORT + 1))" >"$WORKDIR/registry.log" 2>&1 &
PIDS+=($!)

mkdir -p "$DOMAIN"
# writer <flow> <audio flow> <scan> <log>: 480x272, 25 frames per second, for 300 s.
writer() {
  "$WRITER" "$DOMAIN" "$DOMAIN_ID" "$1" "$2" 300 "$WIDTH" "$HEIGHT" 25 "$3" >"$WORKDIR/$4" 2>&1 &
  PIDS+=($!)
}
writer "$TFF" 20000000-0000-4000-8000-000000000001 tff writer-tff.log
writer "$BFF" 20000000-0000-4000-8000-000000000002 bff writer-bff.log
writer "$LIE" 20000000-0000-4000-8000-000000000003 tff writer-lie.log
LIE_WRITER=$!
writer "$FRAMES" 20000000-0000-4000-8000-000000000005 p writer-frames.log
# Channel 5: a domain whose flows keep 60 ms (3 fields at 50 per second).
mkdir -p "$SHORT_DOMAIN"
echo '{"urn:x-mxl:option:history_duration/v1.0":60000000}' >"$SHORT_DOMAIN/options.json"
"$WRITER" "$SHORT_DOMAIN" "$SHORT_DOMAIN_ID" "$SHORT" 20000000-0000-4000-8000-000000000006 300 "$WIDTH" "$HEIGHT" 25 tff >"$WORKDIR/writer-short.log" 2>&1 &
SHORT_WRITER=$!
PIDS+=($SHORT_WRITER)

for _ in $(seq 1 50); do
  [[ -f "$DOMAIN/$LIE.mxl-flow/flow_def.json" && -f "$DOMAIN/$FRAMES.mxl-flow/flow_def.json" ]] && break
  sleep 0.1
done
# Channel 3: the definition now says progressive at 50 grains per second, so a frame is 272 lines;
# the grains still hold 136 (one field).
python3 - "$DOMAIN/$LIE.mxl-flow/flow_def.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
doc["interlace_mode"] = "progressive"
doc["grain_rate"] = {"numerator": 50, "denominator": 1}
json.dump(doc, open(sys.argv[1], "w"))
PY
# Channel 4: whole-frame grains (a progressive flow at 25 grains per second) under a definition that now
# says interlaced_tff. The grains are larger than a field, so only MXL's own grain rate (25, not 50)
# shows that they are not fields: format_mismatch.
python3 - "$DOMAIN/$FRAMES.mxl-flow/flow_def.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
doc["interlace_mode"] = "interlaced_tff"
json.dump(doc, open(sys.argv[1], "w"))
PY

export MXL_DOMAIN_SCAN_PATH="$WORKDIR/mxl"
export MONITOR_CHANNELS=5
export HOST_ID=integration
export NMOS_SEED=integration-interlaced
export NMOS_ENABLE=true
export NMOS_REGISTRY_ADDRESS=127.0.0.1
export NMOS_REGISTRY_PORT="$REG_PORT"
export NMOS_DNS_SD=false
export NMOS_PORT="$NMOS_PORT"
export WEB_PORT="$WEB_PORT"
export MEDIAMTX_RTSP_PORT="$RTSP_PORT"
export MEDIAMTX_API_URL="http://127.0.0.1:${API_PORT}"
export MEDIAMTX_CONFIG_PATH="$WORKDIR/mediamtx.yml"
export MEDIAMTX_WHEP_PORT="$WHEP_PORT"
export MEDIAMTX_HLS_PORT="$HLS_PORT"
export MEDIAMTX_ICE_UDP_PORT="$ICE_PORT"
unset MONITOR_PUBLIC_IP || true
export ENCODER=x264
# A field is 136 lines: the preview takes the first field alone.
export MONITOR_PREVIEW_HEIGHT=136
export MONITOR_VIDEO_BITRATE_KBPS=1500
export CH1_OVERLAY=false CH2_OVERLAY=false CH3_OVERLAY=false CH4_OVERLAY=false CH5_OVERLAY=false
export LOG_LEVEL=info
export READ_OFFSET_GRAINS=2
export STATE_DIR="$WORKDIR/state"
export SHUTDOWN_TIMEOUT_S=10

"$BIN" >"$WORKDIR/monitor.log" 2>&1 &
MONITOR_PID=$!
PIDS+=("$MONITOR_PID")

api() { curl -sf "http://127.0.0.1:${WEB_PORT}$1"; }
alive() {
  if ! kill -0 "$MONITOR_PID" 2>/dev/null; then
    wait "$MONITOR_PID" && code=0 || code=$?
    echo "monitor exited ($code)" >&2
    tail -40 "$WORKDIR/monitor.log" >&2
    exit 1
  fi
}
for _ in $(seq 1 80); do
  alive
  api /livez >/dev/null 2>&1 && break
  sleep 0.25
done
for _ in $(seq 1 80); do
  curl -sf "http://127.0.0.1:${NMOS_PORT}/x-nmos/node/v1.3/receivers" >"$WORKDIR/receivers.json" 2>/dev/null && break
  sleep 0.25
done

# route <channel> <flow> [domain] [sender]: IS-05 PATCH of the channel's video receiver.
route() {
  local channel="$1" flow="$2" domain="${3:-$DOMAIN_ID}" sender="${4:-}" rx
  rx="$(python3 -c 'import json,sys; print(next(r["id"] for r in json.load(open(sys.argv[1])) if r["label"] == "Monitor %s Video" % sys.argv[2]))' "$WORKDIR/receivers.json" "$channel")"
  curl -sf -X PATCH "http://127.0.0.1:${NMOS_PORT}/x-nmos/connection/v1.2/single/receivers/${rx}/staged" \
    -H 'Content-Type: application/json' \
    -d "{\"master_enable\":true,${sender:+\"sender_id\":\"$sender\",}\"activation\":{\"mode\":\"activate_immediate\"},\"transport_params\":[{\"mxl_domain_id\":\"${domain}\",\"mxl_flow_id\":\"${flow}\"}]}" >/dev/null
}
SENDER1="30000000-0000-4000-8000-000000000001"
route 1 "$TFF" "$DOMAIN_ID" "$SENDER1"
route 2 "$BFF"
route 3 "$LIE"
route 4 "$FRAMES"
route 5 "$SHORT" "$SHORT_DOMAIN_ID"

# field <channel> <name>: a field of the channel's status (video.<x> for the video leg).
field() {
  api /api/v1/channels | python3 -c '
import json, sys
c = json.load(sys.stdin)["channels"][int(sys.argv[1]) - 1]
v = c
for part in sys.argv[2].split("."):
    v = v.get(part) if isinstance(v, dict) else None
print(v)' "$1" "$2"
}
metric() { api /metrics | awk -v m="$1" '$1 == m { print $2 }'; }

want_state() {
  local channel="$1" want="$2" state=""
  for _ in $(seq 1 80); do
    alive
    state="$(field "$channel" video.state 2>/dev/null || true)"
    [[ "$state" == "$want" ]] && { echo "ch$channel $state"; return 0; }
    sleep 0.25
  done
  echo "ch$channel: expected $want, got ${state:-none}" >&2
  api /api/v1/channels >&2 || true
  tail -30 "$WORKDIR/monitor.log" >&2
  exit 1
}
want_state 1 running
want_state 2 running
want_state 3 unsupported
want_state 4 unsupported
want_state 5 no_signal

check_eq() {
  [[ "$1" == "$2" ]] || { echo "$3: expected $2, got $1" >&2; exit 1; }
}
check_eq "$(field 1 format)" "272i50" "ch1 format"
check_eq "$(field 1 interlace_mode)" "interlaced_tff" "ch1 interlace_mode"
check_eq "$(field 2 interlace_mode)" "interlaced_bff" "ch2 interlace_mode"
check_eq "$(field 1 field_preview)" "first_field" "ch1 field_preview"
check_eq "$(field 3 video.reason)" "grain_too_small" "ch3 reason"
check_eq "$(field 3 interlace_mode)" "progressive" "ch3 interlace_mode"
check_eq "$(field 4 video.reason)" "format_mismatch" "ch4 reason"
check_eq "$(field 4 interlace_mode)" "interlaced_tff" "ch4 interlace_mode"

# The field rate in grains, the frame rate at the encoder.
rates() {
  local channel="$1" g0 g1
  sleep 2
  g0="$(metric "mxl_webrtc_monitor_grains_read_total{channel=\"$channel\"}")"
  sleep 4
  g1="$(metric "mxl_webrtc_monitor_grains_read_total{channel=\"$channel\"}")"
  local fps
  fps="$(metric "mxl_webrtc_monitor_encode_fps{channel=\"$channel\"}")"
  python3 - "$channel" "$g0" "$g1" "$fps" <<'PY'
import sys
ch, g0, g1, fps = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
grains = (g1 - g0) / 4
print(f"ch{ch}: {grains:.1f} grains/s read, encoder {fps:.1f} fps")
if not 42 <= grains <= 58:
    raise SystemExit(f"ch{ch}: grains per second {grains} (expected the field rate, 50)")
if not 22 <= fps <= 28:
    raise SystemExit(f"ch{ch}: encode fps {fps} (expected the frame rate, 25)")
PY
}
rates 1
rates 2

# Channel 5 is read too late every time: a slate is published and each retry waits one field.
check_eq "$(field 5 preview.state)" "publishing" "ch5 preview (slate)"
r0="$(metric 'mxl_webrtc_monitor_resyncs_total{channel="5"}')"
sleep 3
r1="$(metric 'mxl_webrtc_monitor_resyncs_total{channel="5"}')"
python3 - "$r0" "$r1" <<'PY'
import sys
rate = (float(sys.argv[2]) - float(sys.argv[1])) / 3
print(f"ch5: {rate:.0f} too-late retries per second")
if not 1 <= rate <= 60:
    raise SystemExit(f"ch5: {rate} too-late retries per second (expected at most one per field, 50)")
PY
check_eq "$(field 5 video.state)" "no_signal" "ch5 state"
# The slate goes out once per frame: the encoder and the stream at 25 fps (not one picture per retry).
fps5="$(metric 'mxl_webrtc_monitor_encode_fps{channel="5"}')"
python3 -c 'import sys; f = float(sys.argv[1]); print(f"ch5: encoder {f:.1f} fps (slate)"); sys.exit(0 if 22 <= f <= 28 else f"ch5: slate encoded at {f} fps, expected 25")' "$fps5"

# grab <channel> <name>: decode the channel's RTSP stream for 5 s; prints the stream's frame rate (its
# timestamps) and the mean luma of the upper and the lower half's middle band of the last picture.
grab() {
  local channel="$1" name="$2"
  timeout -k 2 -s INT 5 gst-launch-1.0 -v rtspsrc "location=rtsp://127.0.0.1:${RTSP_PORT}/mxl-webrtc-monitor/ch${channel}" protocols=tcp latency=0 \
    ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! video/x-raw,format=GRAY8 ! identity silent=false \
    ! filesink "location=$WORKDIR/$name.gray" >"$WORKDIR/$name.log" 2>&1 || true
  python3 - "$WORKDIR/$name" <<'PY'
import os, re, sys
base = sys.argv[1]
log = open(base + ".log", errors="replace").read()
line = next((l for l in log.splitlines() if "filesink0.GstPad:sink: caps" in l and "GRAY8" in l), None)
if line is None:
    raise SystemExit("no decoded picture in " + base + ".log")
w = int(re.search(r"width=\(int\)(\d+)", line).group(1))
h = int(re.search(r"height=\(int\)(\d+)", line).group(1))
n = os.path.getsize(base + ".gray") // (w * h)
def seconds(text):
    a, b, c = text.split(":")
    return int(a) * 3600 + int(b) * 60 + float(c)
pts = [seconds(p) for p in re.findall(r"chain +\*+ \(identity0:sink\) \(\d+ bytes, dts: [^,]*, pts: ([0-9:.]+)", log)]
if n < 10 or len(pts) < 10:
    raise SystemExit(f"{base}: {n} pictures")
rate = (len(pts) - 1) / (pts[-1] - pts[0])
data = open(base + ".gray", "rb").read()
frame = data[(n - 1) * w * h:n * w * h]
def band(y0, y1):
    rows = [frame[y * w + w // 4:y * w + 3 * w // 4] for y in range(y0, y1)]
    return sum(sum(r) for r in rows) / sum(len(r) for r in rows)
upper = band(h // 8, 3 * h // 8)
lower = band(5 * h // 8, 7 * h // 8)
# The upper band's even and odd picture lines (a woven frame: one field each).
even = sum(band(y, y + 1) for y in range(h // 8 + h // 8 % 2, 3 * h // 8, 2)) / len(range(h // 8 + h // 8 % 2, 3 * h // 8, 2))
odd = sum(band(y, y + 1) for y in range(h // 8 + 1 - h // 8 % 2, 3 * h // 8, 2)) / len(range(h // 8 + 1 - h // 8 % 2, 3 * h // 8, 2))
print(f"{w} {h} {rate:.2f} {upper:.1f} {lower:.1f} {even:.1f} {odd:.1f}")
PY
}

# expect <channel> <name> <height> <upper> <lower> [<even> <odd>]: frame rate 25, the picture height,
# both bands, and (a woven frame) the even and the odd lines of the upper band.
expect() {
  local out
  out="$(grab "$1" "$2")"
  echo "ch$1 $2: ${out}"
  python3 - "$1" "$2" "$3" "$4" "$5" "${6:-}" "${7:-}" $out <<'PY'
import sys
ch, name, height, upper, lower, even, odd, w, h, rate, got_upper, got_lower, got_even, got_odd = sys.argv[1:]
if int(h) != int(height):
    raise SystemExit(f"ch{ch} {name}: picture height {h}, expected {height}")
if not 24.0 <= float(rate) <= 26.0:
    raise SystemExit(f"ch{ch} {name}: stream at {rate} frames per second, expected 25")
for what, got, want in (("upper", got_upper, upper), ("lower", got_lower, lower)):
    if abs(float(got) - float(want)) > 12:
        raise SystemExit(f"ch{ch} {name}: {what} half luma {got}, expected about {want}")
if even:
    for what, got, want in (("even", got_even, even), ("odd", got_odd, odd)):
        if abs(float(got) - float(want)) > 20:
            raise SystemExit(f"ch{ch} {name}: upper band {what} lines {got}, expected about {want} (field order)")
PY
}
# The writer's frame: upper half 800 (first field) and 560 (second), lower half 320 and 160 (10 bit),
# that is 200/140 and 80/40 in 8 bit. The first field alone: 200 above, 80 below, tff and bff.
# (1.4.0 showed the field squashed into the upper half: black below.)
expect 1 tff-field 136 200 80
expect 2 bff-field 136 200 80
# Channel 5's slate (studio black) as a 25 fps stream.
expect 5 slate 136 16 16

# At the frame's height: the two fields woven. The bands average both fields; the first field is on
# the even lines for tff (ch1) and on the odd lines for bff (ch2).
for channel in 1 2; do
  curl -sf -X PATCH "http://127.0.0.1:${WEB_PORT}/api/v1/channels/${channel}" -H 'Content-Type: application/json' -d '{"preview_height":272}' >/dev/null
done
for channel in 1 2; do
  for _ in $(seq 1 40); do
    [[ "$(field "$channel" field_preview)" == "weave" && "$(field "$channel" preview.state)" == "publishing" ]] && break
    sleep 0.25
  done
  check_eq "$(field "$channel" field_preview)" "weave" "ch$channel field_preview at 272 lines"
done
sleep 2
expect 1 tff-weave 272 170 60 200 140
expect 2 bff-weave 272 170 60 140 200
rates 1

# Channel 3: counted and logged once, although the reader opens the flow again every 200 ms.
alive
check_eq "$(metric 'mxl_webrtc_monitor_video_unsupported_total{channel="3",reason="grain_too_small"}')" "1" "ch3 unsupported_total"
check_eq "$(metric 'mxl_webrtc_monitor_channel_state{channel="3",kind="video",state="unsupported"}')" "1" "ch3 state metric"
check_eq "$(metric 'mxl_webrtc_monitor_video_unsupported_total{channel="4",reason="format_mismatch"}')" "1" "ch4 unsupported_total"
check_eq "$(grep -c '"event":"video_unsupported"' "$WORKDIR/monitor.log")" "2" "video_unsupported log lines (ch3, ch4)"
check_eq "$(metric 'mxl_webrtc_monitor_video_unsupported_total{channel="1",reason="grain_too_small"}')" "0" "ch1 unsupported_total"

# The writer re-creates the flow with a definition that fits its grains: channel 3 reads it.
kill "$LIE_WRITER" 2>/dev/null || true
wait "$LIE_WRITER" 2>/dev/null || true
rm -rf "$DOMAIN/$LIE.mxl-flow"
writer "$LIE" 20000000-0000-4000-8000-000000000004 tff writer-lie2.log
want_state 3 running
check_eq "$(field 3 interlace_mode)" "interlaced_tff" "ch3 interlace_mode after the flow was re-created"
# The new format rebuilt the channel's pipeline: wait for its publish.
sleep 1
for _ in $(seq 1 40); do
  [[ "$(field 3 preview.state)" == "publishing" ]] && break
  sleep 0.25
done
sleep 1
expect 3 recovered 136 200 80

# Channel 5 recovers without a pipeline rebuild: it is routed to a flow of the same format in a domain
# that keeps 200 ms. The picture comes back at 25 fps; slate timestamps that ran ahead of real time
# would hold it back.
builds5="$(grep -c '"event":"pipeline_playing","channel":"5"' "$WORKDIR/monitor.log")"
SHORT_FIXED="10000000-0000-4000-8000-000000000008"
writer "$SHORT_FIXED" 20000000-0000-4000-8000-000000000008 tff writer-short-fixed.log
FIXED_WRITER=$!
for _ in $(seq 1 50); do
  [[ -f "$DOMAIN/$SHORT_FIXED.mxl-flow/flow_def.json" ]] && break
  sleep 0.1
done
route 5 "$SHORT_FIXED"
want_state 5 running
check_eq "$(grep -c '"event":"pipeline_playing","channel":"5"' "$WORKDIR/monitor.log")" "$builds5" "ch5 pipeline builds (no rebuild on recovery)"
rates 5
expect 5 short-recovered 136 200 80

# recover_after <writer pid> <dir> <command...>: the writer stops (killed, as in a crash), ch5 goes to
# no_signal, <dir> is deleted and the writer starts again with the same ids; prints how long ch5 took
# to run again and expects at most 2 s (the reader notices the stopped head after 3 frames and asks MXL
# whether the flow on disk is still the one it reads).
# <max reopens> is how many flow_reopen lines ch5 may log for it: none while the killed writer's flow
# still exists, one when the flow is deleted and made again.
reopens5() { grep -c '"event":"flow_reopen","channel":"5"' "$WORKDIR/monitor.log" || true; }
recover_after() {
  local pid="$1" dir="$2" min="$3" max="$4"
  shift 4
  local before
  before="$(reopens5)"
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  want_state 5 no_signal
  sleep 0.5
  check_eq "$(reopens5)" "$before" "ch5 flow_reopen lines while the stopped writer's flow still exists"
  rm -rf "$dir"
  "$@"
  local t0 t1
  t0="$(date +%s%N)"
  for _ in $(seq 1 100); do
    [[ "$(field 5 video.state)" == "running" ]] && break
    sleep 0.05
  done
  t1="$(date +%s%N)"
  check_eq "$(field 5 video.state)" "running" "ch5 after its flow was made again"
  python3 -c 'import sys; s = (int(sys.argv[2]) - int(sys.argv[1])) / 1e9; print(f"ch5 running again {s:.2f} s after the writer restarted"); sys.exit(0 if s <= 2.0 else "too slow")' "$t0" "$t1"
  local n=$(( $(reopens5) - before ))
  echo "ch5: $n flow_reopen"
  [[ "$n" -ge "$min" && "$n" -le "$max" ]] || { echo "ch5: $n flow_reopen lines, expected $min to $max" >&2; exit 1; }
}

# A writer restart that deletes the flow and makes it again under the same id, in the same domain
# (mxl-test-player keeps its flow ids). 1.4.0 kept reading the deleted flow: no_signal for good.
restart_fixed() {
  writer "$SHORT_FIXED" 20000000-0000-4000-8000-000000000008 tff writer-short-fixed2.log
}
recover_after "$FIXED_WRITER" "$DOMAIN/$SHORT_FIXED.mxl-flow" 1 1 restart_fixed
check_eq "$(grep -c '"event":"pipeline_playing","channel":"5"' "$WORKDIR/monitor.log")" "$builds5" "ch5 pipeline builds (same format, no rebuild)"
expect 5 same-id 136 200 80

# The same with the domain directory made again (a writer that removes its own domain on exit),
# here in the short domain, now keeping 200 ms.
route 5 "$SHORT" "$SHORT_DOMAIN_ID"
restart_short() {
  mkdir -p "$SHORT_DOMAIN"
  echo '{"urn:x-mxl:option:history_duration/v1.0":200000000}' >"$SHORT_DOMAIN/options.json"
  "$WRITER" "$SHORT_DOMAIN" "$SHORT_DOMAIN_ID" "$SHORT" 20000000-0000-4000-8000-000000000006 300 "$WIDTH" "$HEIGHT" 25 tff >"$WORKDIR/writer-short2.log" 2>&1 &
  PIDS+=($!)
}
# The domain scan may notice the missing directory first (no flow_reopen) or the probe (one).
recover_after "$SHORT_WRITER" "$SHORT_DOMAIN" 0 1 restart_short
expect 5 domain-again 136 200 80

alive
kill -TERM "$MONITOR_PID"
wait "$MONITOR_PID" && CODE=0 || CODE=$?
check_eq "$CODE" "143" "exit status on SIGTERM"

# IS-04 and IS-05 agree after a restart that restores the routes from is05.json (1.4.1: 1.4.0 left
# the IS-04 subscription inactive and without the sender).
"$BIN" >"$WORKDIR/monitor2.log" 2>&1 &
MONITOR_PID=$!
PIDS+=("$MONITOR_PID")
for _ in $(seq 1 80); do
  alive
  curl -sf "http://127.0.0.1:${NMOS_PORT}/x-nmos/node/v1.3/receivers" >"$WORKDIR/receivers2.json" 2>/dev/null && break
  sleep 0.25
done
RX1="$(python3 -c 'import json,sys; print(next(r["id"] for r in json.load(open(sys.argv[1])) if r["label"] == "Monitor 1 Video"))' "$WORKDIR/receivers2.json")"
curl -sf "http://127.0.0.1:${NMOS_PORT}/x-nmos/connection/v1.2/single/receivers/${RX1}/active" >"$WORKDIR/active1.json"
python3 - "$WORKDIR/receivers2.json" "$WORKDIR/active1.json" "$RX1" "$SENDER1" <<'PY'
import json, sys
receivers, active, rx, sender = json.load(open(sys.argv[1])), json.load(open(sys.argv[2])), sys.argv[3], sys.argv[4]
sub = next(r for r in receivers if r["id"] == rx)["subscription"]
print(f"ch1 after restart: IS-05 active master_enable={active['master_enable']} sender_id={active['sender_id']}; IS-04 subscription {sub}")
if not (active["master_enable"] is True and active["sender_id"] == sender):
    raise SystemExit("IS-05 /active was not restored")
if not (sub.get("active") is True and sub.get("sender_id") == sender):
    raise SystemExit("the IS-04 subscription does not match IS-05 /active")
others = [r for r in receivers if r["label"].endswith("Audio")]
if any(r["subscription"].get("active") for r in others):
    raise SystemExit("an unrouted receiver's IS-04 subscription is active")
PY
kill -TERM "$MONITOR_PID"
wait "$MONITOR_PID" && CODE=0 || CODE=$?
check_eq "$CODE" "143" "exit status on SIGTERM after the restart"
echo "interlaced integration ok"
