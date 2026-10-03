# mxl-webrtc-monitor — Specification

Status: Draft v0.1 (for implementation by Claude Code in a new, empty repository)
Repository: `LeeO86/mxl-webrtc-monitor` (name can still change)
Sibling projects this spec aligns with: `LeeO86/mxl-decklink`, `LeeO86/mxl-st2110-gateway`,
`LeeO86/mxl-fabrics-agent`, and the platform meta repo `mxl-poc-platform`

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.

---

## 1. Purpose and scope

`mxl-webrtc-monitor` is a media function that lets an operator watch and listen to
MXL flows in a web browser. It exposes N monitor channels as NMOS BCP-007-03 MXL
receivers. A controller (e.g. the Qvest NMOS crosspoint) routes any MXL sender to
a channel with ordinary IS-05. The container reads the flow from the local MXL
domain, encodes a low-bitrate preview, and serves it via WebRTC (WHEP) with HLS as
a fallback, through a mediamtx sidecar. A built-in web page shows all channels as a
multiviewer grid with labels, status and audio meters.

Because each channel is a real NMOS receiver, `mxl-fabrics-agent` sees the
subscription and replicates a flow from another host automatically. The monitor
itself is unaware of hosts and RDMA.

Design principles:

1. **Standards on the control surface.** IS-04 v1.3, IS-05 v1.2, BCP-007-03,
   BCP-004-01 receiver capabilities. No proprietary control needed.
2. **Monitoring quality, not broadcast quality.** Preview resolution, low bitrate,
   sub-second latency target. Not timing-critical; GStreamer is acceptable here.
3. **Never disturb the source.** Pure MXL reader. A slow or stalled monitor MUST
   NOT affect writers or other readers.
4. **Conventions of the sibling repos**: config model, admin UI, `/metrics`, exit
   codes, CI, Compose and Kubernetes as first-class deployments.

Out of scope for v1: composite mosaic as one stream, recording, ST 2110 inputs,
authentication, ANC/data flows (accepted for routing but not displayed; see §5.4),
routing from within the monitor's own UI.

---

## 2. Architecture

```
           NMOS controller (crosspoint)
                    │ IS-05 PATCH (mxl_domain_id, mxl_flow_id)
┌───────────────────▼──────────────────────────────────────────────┐
│ mxl-webrtc-monitor (C++)                                          │
│  nmos-cpp Node: per channel 1 video + 1 audio receiver (grouped)  │
│  per channel:                                                     │
│   MXL reader (C API) ─► appsrc ─► scale ─► overlay ─► H.264 enc ─┐│
│   MXL reader (audio) ─► appsrc ─► select pair ─► level ─► Opus ──┤│
│                                                 rtspclientsink ◄─┘│
│  web server: UI, REST, WebSocket (status, meters), /metrics       │
└───────────────────────────────┬──────────────────────────────────┘
                                │ RTSP on localhost (one path per channel)
┌───────────────────────────────▼──────────────────────────────────┐
│ mediamtx sidecar: WHEP (WebRTC), HLS (low-latency), metrics, API  │
└──────────────────────────────────────────────────────────────────┘
                                │
                             browser
```

- One container for the application, one for mediamtx (official image, pinned).
  In Kubernetes both run in the same pod; in Compose as two services sharing the
  host network.
- The application owns the mediamtx configuration file (generated on start into a
  shared volume) and uses the mediamtx API on localhost for viewer counts and
  path state.

---

## 3. Technology and build

- C++20, CMake ≥ 3.24, Ninja; GCC ≥ 12 or Clang ≥ 16.
- MXL: `dmf-mxl/mxl` pinned to the same revision as `mxl-fabrics-agent`
  (`release/v1.1` at `218ddaa` at the time of writing), built with
  `-DMXL_ENABLE_FABRICS_OFI=OFF`. The pin is one variable in Dockerfile and CI.
- nmos-cpp: same commit as mxl-decklink and mxl-fabrics-agent.
- GStreamer ≥ 1.24 from the base image's packages: core, base (appsrc, videoscale,
  videoconvert, audioconvert, level, opus), good (rtsp client sink is in
  gst-rtsp-server / plugins as packaged), bad (nvcodec for NVENC), ugly (x264).
  Claude Code MUST verify the exact package set for the chosen base image and
  record it.
- MXL is read with the **MXL C API** and pushed into GStreamer via `appsrc`, not
  with the `gst-mxl-rs` `mxlsrc` element. Reason: one MXL pin shared with the
  other repos, and full control over retry, no-signal and index behaviour (§5).
  If this proves impractical, using `mxlsrc` is an allowed deviation that MUST be
  recorded in `IMPLEMENTATION_PLAN.md`.
- Web UI: Vue 3 SPA embedded in the binary (as in mxl-decklink). Players: WHEP
  via the browser's WebRTC API; HLS fallback via hls.js, bundled (no CDN; the lab
  is behind a corporate proxy).
- Base image: Ubuntu 24.04. GPU support via the NVIDIA container runtime; the image
  MUST start and work without a GPU (CPU encoding).
- mediamtx: `bluenviron/mediamtx` official image, exact version pinned.
- Tests: doctest (vendored), shell integration tests.
- Follow the actual headers of the pinned MXL and nmos-cpp versions; record every
  deviation from this spec in `IMPLEMENTATION_PLAN.md`.

Repository layout mirrors the siblings (`.github/workflows`, `cmake`, `deploy`,
`docker`, `src`, `tests`, `third_party`, `web`, `AGENTS.md`,
`IMPLEMENTATION_PLAN.md`, `README.md`, `SPECIFICATION.md`, `LICENSE` MIT).

---

## 4. NMOS

### 4.1 Node and resources

- One nmos-cpp Node per container, one Device. The node label is `NMOS_LABEL`, or `HOST_ID` when that is empty. The device label is `MXL WebRTC Monitor`, or `<NMOS_LABEL> WebRTC Monitor` when the label is set.
- `NMOS_TAGS` is a JSON object of tag name to array of strings. Those tags are added to the node and device. Receiver group hints stay.
- `MONITOR_CHANNELS` (default 4, max 16) channels. Each channel has:
  - one video receiver, `transport: urn:x-nmos:transport:mxl`, format
    `urn:x-nmos:format:video`;
  - one audio receiver, `transport: urn:x-nmos:transport:mxl`, format
    `urn:x-nmos:format:audio`.
  - The two share a group hint `urn:x-nmos:tag:grouphint/v1.0` of
    `Monitor <n>:Video` / `Monitor <n>:Audio`, so controllers can treat them as one
    channel.
- Receiver labels: `Monitor <n> Video` / `Monitor <n> Audio` by default,
  configurable.
- Receiver capabilities (BCP-004-01): video `media_type` `video/v210` (and
  `video/v210a` if supported by the pinned MXL; key is ignored), any resolution and
  rate listed in a configurable set (default: 1080i/p and 2160p at 25/29.97/50/59.94);
  audio `audio/float32`, 1 to 64 channels, 48 kHz.
- Registration: static registry address (`NMOS_REGISTRY_ADDRESS` / `_PORT`).
  The Query API is `NMOS_QUERY_ADDRESS` (default: the registry address) on
  `NMOS_QUERY_PORT` (default: registration port + 1). DNS-SD browsing and mDNS
  advertisement are off by default (`NMOS_DNS_SD=false`), which sets nmos-cpp
  `pri` and `highest_pri` to `INT_MAX`. Avahi is not required in that mode.
- The address announced on the node href, `api.endpoints[].host` and IS-05
  control hrefs is `NMOS_HOST_ADDRESS` (default: `MONITOR_PUBLIC_IP`). It must
  be an IP literal and must not be loopback or `0.0.0.0`.
- Stable IDs: Node, Device and Receiver IDs are derived deterministically (UUIDv5)
  from a configurable seed (`NMOS_SEED`, default `HOST_ID-monitor`) and
  the channel number, so they survive restarts and the crosspoint keeps its view.
  This process has no sources, flows or senders, and it does not create an output domain.

### 4.2 IS-05 behaviour

- Staged/active per BCP-007-03: `transport_params[0]` with `mxl_domain_id` and
  `mxl_flow_id`; `transport_file` not used.
- **Activation is accepted even if the domain or flow is not on disk yet.** The
  channel goes to state `waiting` and retries (§5.2). This deliberately differs
  from mxl-decklink and mxl-st2110-gateway, so the monitor also works with
  `mxl-fabrics-agent` in `on-demand` mirror mode.
- Rejection only for malformed parameters (non-UUID values) with the standard IS-05
  error responses.
- `master_enable: false` stops the channel's pipeline and shows the "not routed"
  state.
- The active connection is stored in `STATE_DIR/is05.json` and restored on the
  next start. A missing or corrupt file is ignored.
- The IS-04 receiver `subscription` (`sender_id`, `active`) is updated on every
  activation, so the fabrics agent and the crosspoint see it.
- Video and audio receivers of a channel are independent: audio may come from a
  different sender than video.

---

## 5. Media path

### 5.1 Domain resolution

- `MXL_DOMAIN_SCAN_PATH` (default `/Volumes/mxl`): direct subdirectories with a
  `domain_def.json` are domains; identity is the `id` field, never the directory
  name. Mirror domains created by the fabrics agent (`mirror-<id>`, with an
  `x-mxl-fabrics-agent` object in `domain_def.json`) are treated like any other
  domain; unknown fields are ignored.
- Rescan on every resolution attempt; no negative caching.
- The MXL root is mounted read-only; the monitor never creates domains or flows.

### 5.2 Reader lifecycle

- On activation: resolve domain → open MXL instance for that domain → open flow
  reader. Each step that fails because something does not exist yet retries with
  exponential backoff (250 ms → 5 s) while `master_enable` is true. State
  `waiting` with the reason (`domain_not_found`, `flow_not_found`).
- A flow that exists but has no new grains is state `no_signal` (not an error).
  The pipeline keeps running with a slate (§5.3) and resumes when grains arrive.
- Reading position: the reader reads `READ_OFFSET_GRAINS` (default 2) behind the
  flow's head index. For a flow on a mirror domain this keeps the reader behind the
  replication head (see the fabrics agent's `replication_lag_grains`). Audio reads
  the matching sample window aligned to the same TAI time.
- If the reader falls behind beyond the ring (too late), it resynchronises to head
  minus offset and counts a `resync` (metric).
- Format change of the source (new flow or changed `flow_def.json`): the channel
  rebuilds its pipeline. If the source mints a new flow id, the old id goes to
  `waiting`; following the new id is the controller's job (the crosspoint does
  this for MXL receivers).
- The reader runs on its own thread per flow; GStreamer pushes never block the
  reader thread for longer than one grain (bounded `appsrc` queue, leaky: drop
  oldest), so a slow encoder cannot stall MXL reading.

### 5.3 Video processing

- v210 → raw video via the pinned MXL/GStreamer conversion path (CPU unpack, or a
  GPU path if available and simple; record the choice).
- Scaling to the channel's preview size (`MONITOR_PREVIEW_HEIGHT`, default 540,
  aspect preserved); interlaced sources are deinterlaced (simple, e.g. bob or
  yadif-equivalent available in GStreamer).
- Optional frame-rate decimation (`MONITOR_MAX_FPS`, default 0 = source rate).
- Overlay (burned in, configurable on/off): channel label, source label (IS-04
  label of the sender, looked up in the registry), format string (e.g. `1080p50`).
- Slate when `waiting`, `no_signal` or `not routed`: static frame with the channel
  label and the state text, at the last known (or default 25p) rate, so the player
  never stalls.

### 5.4 Audio processing

- Float32 input with N channels. Selectable channel pair per monitor channel
  (`audio_pair`, default 1/2), downmix option (`stereo` or `mono` sum).
- Level metering on all input channels (peak and RMS per channel, ~10 Hz) via the
  GStreamer `level` element or own computation; sent to the UI over WebSocket, not
  burned into video.
- Encoded as Opus, 48 kHz stereo, configurable bitrate (default 128 kbit/s).
- If no audio is routed, the channel is video-only (silence is not encoded).
- ANC/data receivers are not exposed in v1.

### 5.5 Encoding

- H.264, no B-frames, GOP 1 s, constant bitrate per channel (default 2 Mbit/s,
  configurable), low-latency tuning.
- Encoder selection `ENCODER=auto|nvenc|x264`: `auto` uses NVENC if an NVIDIA GPU
  is present and the `nvh264enc` element initialises, else x264. The selected
  encoder per channel is reported in status and metrics.
- NVENC session limits: the application MUST handle encoder session creation
  failure by falling back to x264 for that channel and reporting it.

### 5.6 Delivery

- Each channel publishes to mediamtx on localhost via RTSP, path `ch<n>`.
- mediamtx serves:
  - WebRTC via WHEP (`/ch<n>/whep`), single UDP port for ICE plus TCP ICE fallback,
    `webrtcAdditionalHosts` set to the node's management IP (`MONITOR_PUBLIC_IP`);
  - low-latency HLS (fMP4) as fallback;
  - metrics and API, API bound to localhost.
- Claude Code MUST verify that the chosen mediamtx version serves Opus over HLS for
  the fallback; if not, add an AAC track for HLS and record the decision.
- No TURN server in v1. The README documents that browsers need UDP to the node for
  WebRTC and that HLS works over TCP when UDP is blocked.

---

## 6. Web UI and API

### 6.1 Multiviewer page (`/`)

- Grid of all channels (layout 1×1, 2×2, 3×3, 4×4, automatic by default); click a
  tile for full size.
- Per tile: player (WHEP, automatic fallback to HLS after a timeout or ICE
  failure), channel label, routed source label, state badge, format, encoder,
  audio meters (bars per input channel, selected pair highlighted), mute/unmute
  (audio muted by default in the browser).
- Status and meters via one WebSocket (`/api/v1/events`).
- Works without internet access (all assets embedded).

### 6.2 Admin pages

- **Channels:** number of channels (restart required), labels, preview height,
  bitrate, max fps, audio pair and downmix, overlay options.
- **NMOS:** node info, registry status, per-receiver active IS-05 parameters and
  subscription.
- **Settings:** effective configuration; env-set keys read-only; config file import
  and export; `KEY=value` export (as in mxl-decklink).

### 6.3 REST

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/v1/info` | version, MXL/nmos-cpp/GStreamer/mediamtx versions, encoder availability |
| GET | `/api/v1/channels` | channels with state, routing, format, encoder, viewers |
| PATCH | `/api/v1/channels/{n}` | per-channel settings (not routing) |
| GET | `/api/v1/events` | WebSocket: status and meters |
| GET/PUT | `/api/v1/config` | configuration (as siblings) |
| GET | `/api/v1/config.env` | `KEY=value` export |
| GET | `/api/v1/config/export` | one JSON document of settings and channel keys |
| POST | `/api/v1/config/import` | restore that document into `MONITOR_CONFIG_FILE` |
| GET | `/api/v1/nmos` | node id, registry and receiver state |

Routing is only via IS-05. The UI MUST NOT offer a source picker in v1.

### 6.4 Ops endpoints

`/livez`, `/readyz` (ready = MXL root mounted, NMOS registered or disabled,
mediamtx reachable), `/statusz`, `/metrics` — on `WEB_PORT`.

---

## 7. Configuration

Environment variables over an optional JSON file (`MONITOR_CONFIG_FILE`) over
defaults. Unknown environment variables are ignored. Unknown keys in the JSON
file, and invalid values, exit 78 with a message on stderr. This process has
no secrets; they are never logged, and `/api/v1/config/export` sets
`secrets_included` to false. App-written state (`is05.json`, and `mediamtx.yml`
unless `MEDIAMTX_CONFIG_PATH` is set) lives only under `STATE_DIR` (default
`/config`). Global keys changed in the UI are flagged `restart_required`;
per-channel settings apply at runtime. `POST /api/v1/config/import` restores a
document from `GET /api/v1/config/export` into the config file and skips keys
that the environment owns. It returns 409 when `MONITOR_CONFIG_FILE` is unset.

| Key | Default | Meaning |
| --- | --- | --- |
| `HOST_ID` | hostname | used for the NMOS seed and labels |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | MXL root (read-only) |
| `MONITOR_CHANNELS` | 4 | 1–16 |
| `MONITOR_PREVIEW_HEIGHT` | 540 | preview height |
| `MONITOR_MAX_FPS` | 0 | 0 = source rate |
| `MONITOR_VIDEO_BITRATE_KBPS` | 2000 | per channel |
| `MONITOR_AUDIO_BITRATE_KBPS` | 128 | per channel |
| `READ_OFFSET_GRAINS` | 2 | read behind head |
| `ENCODER` | `auto` | `auto`, `nvenc`, `x264` |
| `MONITOR_PUBLIC_IP` | first non-loopback IPv4 | ICE host (`webrtcAdditionalHosts`) and the default for `NMOS_HOST_ADDRESS`. Must be an IP literal, not loopback and not `0.0.0.0` |
| `NMOS_HOST_ADDRESS` | `MONITOR_PUBLIC_IP` | IP announced on the IS-04 href, API endpoints and IS-05 control hrefs |
| `MONITOR_WHEP_PUBLIC_URL` | empty | public base URL of WHEP, no path; empty keeps `http://MONITOR_PUBLIC_IP:MEDIAMTX_WHEP_PORT`. A TLS hostname is allowed here |
| `MONITOR_HLS_PUBLIC_URL` | empty | public base URL of HLS, no path; empty keeps `http://MONITOR_PUBLIC_IP:MEDIAMTX_HLS_PORT`. A TLS hostname is allowed here |
| `STATE_DIR` | `/config` | directory for generated state (`mediamtx.yml` unless overridden, and `is05.json`) |
| `SHUTDOWN_TIMEOUT_S` | 10 | seconds allowed after SIGTERM before exit 143 is forced |
| `MXL_CLEANUP_ON_EXIT` | false | accepted; this monitor owns no output domain, so `true` only logs and deletes nothing |
| `NMOS_LABEL` | empty | node label; device label prefix. Empty keeps `HOST_ID` and `MXL WebRTC Monitor` |
| `NMOS_TAGS` | `{}` | JSON object of tag name to array of strings, on the node and device |
| `MEDIAMTX_RTSP_URL` | `rtsp://127.0.0.1:8554` | sidecar ingest |
| `MEDIAMTX_API_URL` | `http://127.0.0.1:9997` | sidecar API |
| `MEDIAMTX_CONFIG_PATH` | `STATE_DIR/mediamtx.yml` | generated config for the sidecar |
| `MEDIAMTX_WHEP_PORT` / `_HLS_PORT` / `_ICE_UDP_PORT` | 8889 / 8888 / 8189 | written into the generated config |
| `MEDIAMTX_METRICS_PORT` | API port + 1 | sidecar Prometheus port written into the generated config. `0` means the default |
| `NMOS_ENABLE` | true | |
| `NMOS_REGISTRY_ADDRESS` / `_PORT` | empty / 3210 | static registration API |
| `NMOS_QUERY_ADDRESS` / `_PORT` | registry address / registry port + 1 | Query API used for readiness and sender labels |
| `NMOS_DNS_SD` | false | enable DNS-SD discovery and mDNS advertisement |
| `NMOS_PORT` | 3242 | Node API. The Node WebSocket is `NMOS_PORT+1` |
| `NMOS_SEED` | `HOST_ID-monitor` | UUIDv5 seed for the node, device and receivers |
| `WEB_PORT` | 8100 | UI, REST, health, metrics |
| `LOG_LEVEL` | `info` | JSON logs |

Defaults do not collide, under host networking, with mxl-decklink (8080, 3212/3213),
mxl-st2110-gateway (8090), mxl-fabrics-agent (8095, 3232/3233, 23500–23599) and
FlowXer (9620). Running two monitors on one host requires different ports for
both containers; the README documents this.

### 7.1 Behind an HTTPS reverse proxy

The UI, WHEP and HLS can each have their own hostname on a TLS proxy that
only exposes port 443. Set `MONITOR_WHEP_PUBLIC_URL` and
`MONITOR_HLS_PUBLIC_URL` to those absolute origins, for example
`https://mon1-whep.small.mxl.ipla.media.int` and
`https://mon1-hls.small.mxl.ipla.media.int`. The channel API then returns
`<base>/ch<n>/whep` and `<base>/ch<n>/index.m3u8` with `playback.public`
true, and the page uses those URLs unchanged.

Leave both empty to keep today's URLs,
`http://<MONITOR_PUBLIC_IP>:<port>/ch<n>/...`. The page then rewrites only
the hostname to the host that served the UI.

ICE does not go through the proxy. `MONITOR_PUBLIC_IP` is still the address
written into `webrtcAdditionalHosts`, so media UDP (and the TCP fallback)
reaches the node directly. A same-origin reverse proxy inside this process
is not part of v1.

---

## 8. Metrics (prefix `mxl_webrtc_monitor_`)

| Metric | Type | Labels |
| --- | --- | --- |
| `info` | gauge (1) | version, mxl_version, encoder_available |
| `channel_state` | gauge (1 for current state) | channel, kind (video/audio), state |
| `grains_read_total` | counter | channel |
| `grains_dropped_total` | counter | channel, reason (queue_full, too_late) |
| `resyncs_total` | counter | channel |
| `read_lag_grains` | gauge | channel |
| `encode_fps` | gauge | channel |
| `encode_latency_seconds` | histogram | channel, encoder |
| `encoder_fallbacks_total` | counter | channel |
| `output_bitrate_bps` | gauge | channel |
| `viewers` | gauge | channel, protocol (webrtc/hls) |
| `audio_peak_dbfs` | gauge | channel, input_channel (optional, off by default) |

A Grafana dashboard `deploy/grafana/mxl-webrtc-monitor.json` shows states, lag,
drops, encode fps and latency, bitrate and viewers. mediamtx's own metrics are
scraped separately.

---

## 9. Container, deployment, CI

- Runtime requirements: host networking; MXL root mounted read-only; runs as
  `1000:1000` (or `supplementalGroups: [1000]`); GPU optional
  (`runtimeClassName: nvidia`, `nvidia.com/gpu: 1`); no extra capabilities.
- Exit codes: 0 is unused on the signal path, 75 a listening port could not be
  bound or startup failed, 78 invalid configuration, 143 SIGTERM or SIGINT
  completed the shutdown sequence (or `SHUTDOWN_TIMEOUT_S` elapsed). On
  SIGTERM the process stops media and releases MXL readers, erases the NMOS
  node from the model so the registry receives DELETE, logs that it owns no
  output domain when `MXL_CLEANUP_ON_EXIT=true`, and exits 143.
- CI: build, unit and integration tests, GHCR image
  `ghcr.io/leeo86/mxl-webrtc-monitor`. A `vX.Y.Z` tag publishes `X.Y.Z`, `X.Y`
  and `X`. `main` publishes `git-<sha7>` and `nightly-dev`. Image tags are not
  moved except those floating tags. The runtime user is uid 1000. OCI labels
  include `org.opencontainers.image.source`, `.revision`, `.licenses` and
  `io.dmf.mxl.revision` (the pinned MXL commit).
- **Docker Compose** (`docker/`):
  - `docker-compose.demo.yaml`: single machine, no GPU required: nmos-cpp
    registry, a test MXL writer (mxl-decklink in mock mode or the CBC
    `test-generator`), the monitor and mediamtx, Prometheus and Grafana. README
    shows routing a flow to channel 1 with `curl` against IS-05 and opening the
    multiviewer.
  - `docker-compose.host.yaml`: one host of a real platform.
- **Kubernetes** (`deploy/`): Deployment (one replica per node, node-pinned) with
  the app and mediamtx containers in one pod, ConfigMap, optional GPU, probes,
  ServiceMonitor example, Grafana dashboard. Written so `mxl-poc-platform` can
  vendor it.

---

## 10. Testing

- Unit: config precedence, IS-05 parameter validation, deterministic IDs, channel
  state machine (not routed → waiting → no signal → running → waiting), domain
  resolution including mirror domains, audio pair selection.
- Integration (CI, no GPU): a test writer creates a v210 + float32 flow in a temp
  MXL root; a minimal registry stand-in (or nmos-cpp registry container); the test
  PATCHes channel 1, waits for state `running`, then fetches the HLS playlist from
  mediamtx and checks that segments are produced; then stops the writer and checks
  `no_signal`; then activates a non-existent flow and checks `waiting`, creates
  the flow later and checks `running` without further action.
- Hardware check (documented, not in CI): NVENC on A4000 and L4, 4 and 16 channels,
  browser playback via WebRTC from an operator desk, HLS fallback with UDP blocked.

---

## 11. Implementation order (suggested)

1. Skeleton, config, ops endpoints, metrics, CI, Dockerfile.
2. NMOS node with channel receivers and IS-05 handling (no media).
3. MXL reader with state machine and slate; GStreamer pipeline with x264 → RTSP →
   mediamtx; HLS check in integration test.
4. Audio path with metering; Opus.
5. NVENC selection and fallback.
6. Web UI (multiviewer, admin), WebSocket events.
7. Compose demo, Kubernetes manifests, Grafana dashboard, README.

## 12. Open points

- Final repository name.
- Whether `video/v210a` sources should be supported (key ignored) in v1.
- Default channel count per platform (set in `mxl-poc-platform`).
- Whether a TURN server is needed for operator desks (depends on the lab firewall).
