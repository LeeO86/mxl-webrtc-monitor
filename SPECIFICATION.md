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
a fallback, through MediaMTX: its own, built into the image, or a platform's shared one
(§5.7). A built-in web page shows all channels as a multiviewer grid with labels, status
and audio meters; one channel can be embedded in an operator screen (§6.6).

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
                                │ RTSP (one path per channel: <prefix>/ch<n>)
┌───────────────────────────────▼──────────────────────────────────┐
│ MediaMTX: WHEP (WebRTC), HLS (low-latency), metrics, API          │
│  own: a child process of the monitor (binary in the image)        │
│  shared: the platform's, PREVIEW_PUBLISH_URL                       │
└──────────────────────────────────────────────────────────────────┘
                                │
                             browser
```

- One container. In own mode (§5.7) the application starts the MediaMTX binary of
  the image (official release, pinned) as a supervised child process; in shared
  mode it publishes to a MediaMTX it does not start. 1.0 to 1.2 ran MediaMTX as a
  sidecar container in the same pod or Compose project.
- The application owns the MediaMTX configuration file (generated on start) and uses
  the MediaMTX API for viewer counts and path state (own mode; shared mode when
  `MEDIAMTX_API_URL` is set).

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
- MediaMTX: the `/mediamtx` binary (and its MIT `LICENSE`) of the official
  `bluenviron/mediamtx` image, exact version pinned, copied into this image.
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
  the matching sample window aligned to the same TAI time. Audio that arrives later
  than the video (a mirror of another host's flow next to local video) is read at
  its head instead; lip sync is then off by that lag. The stream carries exactly as
  much audio as the pushed video frames cover, read sample by sample after the last
  pushed sample; samples that are not there in time become silence.
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

- Each channel publishes to MediaMTX via RTSP over TCP, path `<PREVIEW_PATH_PREFIX>/ch<n>`
  (§5.7): the built-in one on localhost, or a shared one.
- The built-in MediaMTX serves:
  - WebRTC via WHEP (`/<prefix>/ch<n>/whep`), single UDP port for ICE plus TCP ICE fallback,
    `webrtcAdditionalHosts` set to the node's management IP (`MONITOR_PUBLIC_IP`);
  - low-latency HLS (fMP4) as fallback;
  - metrics and API, API bound to localhost.
- Claude Code MUST verify that the chosen mediamtx version serves Opus over HLS for
  the fallback; if not, add an AAC track for HLS and record the decision.
- No TURN server in v1. The README documents that browsers need UDP to the node for
  WebRTC and that HLS works over TCP when UDP is blocked.

### 5.7 Preview contract (1.3.0, platform §11.5 / D-185)

The platform runs one shared MediaMTX; outside the platform the monitor stays
self-contained. The same settings exist in every repo with browser previews.

- `PREVIEW_PUBLISH_URL` (alias `MEDIAMTX_RTSP_URL`): an `rtsp://` or `rtsps://` base
  without path or credentials. Set: **shared mode**, the channels publish there and
  this process starts no MediaMTX. Empty (default): **own mode**, the process starts
  the image's MediaMTX as a child process (own process group, SIGTERM when the
  monitor dies) with the generated config, restarts it after it exits (1 s, doubling
  to 10 s; 1 s again after 10 s of running), and stops it on shutdown (SIGTERM, then
  SIGKILL after 3 s). Its RTSP ingest is `127.0.0.1:MEDIAMTX_RTSP_PORT`.
- `PREVIEW_PATH_PREFIX`: MediaMTX path segments (`[A-Za-z0-9._~-]`, joined by `/`;
  outer slashes are dropped). Default `mxl-webrtc-monitor`, the function's own name.
  Channel `n` publishes `<prefix>/ch<n>`.
- `PREVIEW_WHEP_URL` / `PREVIEW_HLS_URL` (aliases `MONITOR_WHEP_PUBLIC_URL` /
  `MONITOR_HLS_PUBLIC_URL`): the public bases the page plays from,
  `<base>/<prefix>/ch<n>/whep` and `<base>/<prefix>/ch<n>/index.m3u8`. Empty: the own
  MediaMTX on `MONITOR_PUBLIC_IP` and `MEDIAMTX_WHEP_PORT` / `MEDIAMTX_HLS_PORT`, with
  the hostname replaced by the page's (§7.1).
- Aliases are resolved in each configuration layer before the layers are merged; a
  set (not empty) new name wins over its alias. The configuration API and the export
  show only the new names.
- `MEDIAMTX_API_URL` defaults to `http://127.0.0.1:9997` in own mode and is empty in
  shared mode; when set it gives viewers and path state. `/readyz` needs the MediaMTX
  API only in own mode.
- The config file is written in both modes, so a 1.2.0 sidecar deployment (which
  sets `MEDIAMTX_RTSP_URL`) keeps working in shared mode.
- Publish state per channel: `connecting` until the RTSP sink passes media,
  `publishing` once MediaMTX took ANNOUNCE, SETUP and RECORD and buffers flow (more
  than two buffers through the sink since the stream was built), `error` after a
  pipeline error, with that error, until it publishes again. `/statusz`, the channel
  status and the metrics (§8) show the mode and these states.

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
- 1.1.0 implements these in the look of the sibling UIs (header pills, banners, `#hash` tabs, light
  and dark theme): Multiview, Channels (settings including the overlay parts, IS-05 routes, stream and
  MediaMTX path), NMOS, Status (probes, versions, MediaMTX, `/metrics` counters per channel) and Settings
  (origins, editing of non-environment keys, export, import). Edits are drafts that survive tab switches
  and reconnects. A player starts again only when its URLs, video state, video flow or audio routing
  change, or when a WebRTC session that played drops.

### 6.3 REST

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/v1/info` | version, MXL/nmos-cpp/GStreamer versions, `mediamtx` (the running MediaMTX from its `/v3/info`, `unknown` when unreachable) and `mediamtx_pin` (the version of the examples), encoder availability |
| GET | `/api/v1/channels` | channels with state, routing, format, encoder, viewers |
| PATCH | `/api/v1/channels/{n}` | per-channel settings (not routing) |
| GET | `/api/v1/events` | WebSocket: status and meters |
| GET/PUT | `/api/v1/config` | configuration (as siblings) |
| GET | `/api/v1/config.env` | `KEY=value` export |
| GET | `/api/v1/config/export` | one JSON document of settings and channel keys |
| POST | `/api/v1/config/import` | restore that document into `MONITOR_CONFIG_FILE` |
| GET | `/api/v1/nmos` | node id, registry and receiver state |
| GET | `/widgets` | the operator-screen widgets (§6.6) |
| GET | `/widget/channel?ch=<n>` | one channel as a widget page (§6.6) |

Routing is only via IS-05. The UI MUST NOT offer a source picker in v1.

Additions for the UI (1.1.0): `label` (node label) in `/api/v1/info`; `overlay_label`,
`overlay_source`, `overlay_format` and `mediamtx` (`ready`, `tracks` of the channel's MediaMTX
path) in each channel of `/api/v1/channels` and the events.

Additions for tally (1.2.0, §6.5): `tally`, `tsl_text`, `tsl_lh`, `tsl_rh`, `tsl_text_tally` and the
setting `tally_text` in each channel of `/api/v1/channels` and the events; `PATCH` takes `tally_text`.

Additions for previews (1.3.0, §5.7): `preview` (`path`, `state`, `error`) in each channel of
`/api/v1/channels` and the events; the playback URLs carry the path prefix.

### 6.4 Ops endpoints

`/livez`, `/readyz` (ready = MXL root mounted, NMOS registered or disabled,
the built-in MediaMTX reachable in own mode), `/statusz`, `/metrics` — on `WEB_PORT`.
`/statusz` has `preview`: `mode` (`own` or `shared`), `publish_url`, `path_prefix`, in own
mode `mediamtx` (`running`, `restarts`), and `streams` (`channel`, `path`, `state`, `error`).

### 6.5 Tally (TSL UMD 5.0)

The receiver is mxl-multiviewer's (its SPECIFICATION §7, release 1.3.0), without TSL 3.1.

- `TSL_ENABLE=true` listens on `TSL_UDP_PORT` (default 8912) and `TSL_TCP_PORT` (default 8913).
  It is off by default. A port that cannot be bound exits 75.
- Packet (little-endian): `PBC` (the number of bytes after `PBC`), `VER` 0, `FLAGS` (bit 0: text is
  UTF-16LE, else ASCII; bit 1: screen control data, which carries no displays and is ignored),
  `SCREEN`, then display messages `INDEX`, `CONTROL`, `LENGTH`, `TEXT`. `CONTROL` bits 0–1 are RH,
  2–3 the text tally, 4–5 LH, 6–7 brightness (not used). Bit 15 marks control data; that message is
  skipped. Tally values: 0 off, 1 red, 2 green, 3 amber. UTF-16 text (surrogate pairs included) is
  converted to UTF-8; a lone surrogate, and an ASCII byte above 0x7F, becomes U+FFFD. A packet that
  is shorter than its `PBC` or ends inside a message is dropped as a whole.
- TCP uses DLE/STX framing (DLE 0xFE, STX 0x02; a 0xFE byte in the packet is sent as DLE DLE).
  DLE/ETX (0x03) after the packet is accepted, not required: a packet also ends when it holds
  `PBC` + 2 bytes, or when the next DLE/STX starts. Up to 8 TCP clients. A UDP datagram is one bare
  packet or a packet in the same framing.
- Display index to channel: `TSL_MAP` (`display:channel` pairs, e.g. `0:1,1:2`). Empty means
  display `i` is channel `i+1` (the index is 0-based, as in the multiviewer). `TSL_SCREEN` (default
  −1: every screen) accepts only that screen. A display without a channel is ignored.
- Each channel in `GET /api/v1/channels` and the events carries `tsl_lh`, `tsl_rh` and
  `tsl_text_tally` as received, `tally` (the border colour: the text tally, else RH, else LH) and
  `tsl_text` (UTF-8). They keep the last message for that display and are not saved.
- Web UI: while `TSL_ENABLE` is true, each tile has a left lamp (LH) and a right lamp (RH) at the
  ends of its title; an off lamp is not drawn. The picture gets a border in the `tally` colour. The
  channel setting `tally_text` (`CH<n>_TALLY_TEXT`, default false, applies at once) puts the
  channel label on the text tally colour, with black text, while that is not off. The Channels tab
  lists the fields. The tally is shown in the page only, not burned into the stream.
- The `TSL_*` keys are global settings: changed in the UI, they apply after a restart.

### 6.6 Widgets (1.3.0, operator screens)

The contract is agreed with the platform's production designer.

- `GET /widgets` answers `[{id, title, params, min_size: {w, h}, version}]`; `params` is a
  JSON schema of the widget's query parameters, `version` the monitor version.
- One widget, `channel`: `params` `ch` (integer, 1..`MONITOR_CHANNELS`, required), `labels`
  and `meters` (booleans, default true); `min_size` 320×200.
- `GET /widget/channel?ch=<n>[&labels=][&meters=][&theme=dark|light|transparent]` is a page
  with only that channel's tile, filling its frame: the WHEP picture (HLS fallback, muted)
  from the preview contract (§5.7), the title with the channel label and the TSL lamps
  (`labels`), the tally border and text tally by the rules of §6.5, and the audio meters
  (`meters`). `theme` forces dark or light colours, or dark colours on a transparent
  background; without it the page follows the browser. The page uses the monitor's API on
  its own origin (no CORS). An invalid parameter answers 400, an unknown widget 404.
- The `/widget` routes carry `Content-Security-Policy: frame-ancestors
  <WIDGET_FRAME_ANCESTORS>` (default `'self'`) and no `X-Frame-Options`. The value is a CSP
  source list; `;`, `,` and control characters exit 78.
- The page posts `{type: "widget-ready"}` once the channel is shown, and
  `{type: "widget-size", w, h}` then and on every resize, to `window.parent`.

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
| `PREVIEW_PUBLISH_URL` | empty | RTSP base of a shared MediaMTX; empty runs the built-in one (§5.7). Alias `MEDIAMTX_RTSP_URL` |
| `PREVIEW_PATH_PREFIX` | `mxl-webrtc-monitor` | path prefix of the streams, `<prefix>/ch<n>` |
| `PREVIEW_WHEP_URL` | empty | public base URL of WHEP, no path; empty keeps `http://MONITOR_PUBLIC_IP:MEDIAMTX_WHEP_PORT`. A TLS hostname is allowed here. Alias `MONITOR_WHEP_PUBLIC_URL` |
| `PREVIEW_HLS_URL` | empty | public base URL of HLS, no path; empty keeps `http://MONITOR_PUBLIC_IP:MEDIAMTX_HLS_PORT`. A TLS hostname is allowed here. Alias `MONITOR_HLS_PUBLIC_URL` |
| `WIDGET_FRAME_ANCESTORS` | `'self'` | CSP `frame-ancestors` of the `/widget` routes (§6.6) |
| `STATE_DIR` | `/config` | directory for generated state (`mediamtx.yml` unless overridden, and `is05.json`) |
| `SHUTDOWN_TIMEOUT_S` | 10 | seconds allowed after SIGTERM before exit 143 is forced |
| `MXL_CLEANUP_ON_EXIT` | false | accepted; this monitor owns no output domain, so `true` only logs and deletes nothing |
| `NMOS_LABEL` | empty | node label; device label prefix. Empty keeps `HOST_ID` and `MXL WebRTC Monitor` |
| `NMOS_TAGS` | `{}` | JSON object of tag name to array of strings, on the node and device |
| `MEDIAMTX_RTSP_PORT` | 8554 | RTSP ingest of the built-in MediaMTX on 127.0.0.1 |
| `MEDIAMTX_API_URL` | `http://127.0.0.1:9997` (shared mode: empty) | MediaMTX API for viewers and path state |
| `MEDIAMTX_CONFIG_PATH` | `STATE_DIR/mediamtx.yml` | generated MediaMTX config |
| `MEDIAMTX_WHEP_PORT` / `_HLS_PORT` / `_ICE_UDP_PORT` | 8889 / 8888 / 8189 | written into the generated config |
| `MEDIAMTX_METRICS_PORT` | API port + 1 | MediaMTX Prometheus port written into the generated config. `0` means the default |
| `NMOS_ENABLE` | true | |
| `NMOS_REGISTRY_ADDRESS` / `_PORT` | empty / 3210 | static registration API |
| `NMOS_QUERY_ADDRESS` / `_PORT` | registry address / registry port + 1 | Query API used for readiness and sender labels |
| `NMOS_DNS_SD` | false | enable DNS-SD discovery and mDNS advertisement |
| `NMOS_PORT` | 3242 | Node API. The Node WebSocket is `NMOS_PORT+1` |
| `NMOS_SEED` | `HOST_ID-monitor` | UUIDv5 seed for the node, device and receivers |
| `WEB_PORT` | 8100 | UI, REST, health, metrics |
| `LOG_LEVEL` | `info` | JSON logs |
| `TSL_ENABLE` | false | TSL UMD 5.0 tally receiver (§6.5) |
| `TSL_UDP_PORT` / `TSL_TCP_PORT` | 8912 / 8913 | tally over UDP and TCP |
| `TSL_SCREEN` | −1 | only this screen; −1 every screen |
| `TSL_MAP` | empty | `display:channel` pairs; empty: display `i` is channel `i+1` |

Defaults do not collide, under host networking, with mxl-decklink (8080, 3212/3213),
mxl-st2110-gateway (8090), mxl-fabrics-agent (8095, 3232/3233, 23500–23599),
mxl-multiviewer (8110, 3262/3263, TSL 8910/8911) and FlowXer (9620). Running two
monitors on one host requires different ports for both containers; the README
documents this.

### 7.1 Behind an HTTPS reverse proxy

The UI, WHEP and HLS can each have their own hostname on a TLS proxy that
only exposes port 443. Set `PREVIEW_WHEP_URL` and
`PREVIEW_HLS_URL` to those absolute origins, for example
`https://mon1-whep.small.mxl.ipla.media.int` and
`https://mon1-hls.small.mxl.ipla.media.int`. The channel API then returns
`<base>/<prefix>/ch<n>/whep` and `<base>/<prefix>/ch<n>/index.m3u8` with `playback.public`
true, and the page uses those URLs unchanged.

Leave both empty to keep the direct URLs,
`http://<MONITOR_PUBLIC_IP>:<port>/<prefix>/ch<n>/...`. The page then rewrites only
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
| `preview_mode` | gauge (1 for the mode) | mode (own/shared) |
| `preview_publish_state` | gauge (1 for the current state) | channel, state (connecting/publishing/error) |

A Grafana dashboard `deploy/grafana/mxl-webrtc-monitor.json` shows states, lag,
drops, encode fps and latency, bitrate and viewers. mediamtx's own metrics are
scraped separately.

---

## 9. Container, deployment, CI

- One container: the image carries the MediaMTX binary (§5.7); no sidecar.
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
    `test-generator`), the monitor with its built-in MediaMTX, Prometheus and Grafana. README
    shows routing a flow to channel 1 with `curl` against IS-05 and opening the
    multiviewer.
  - `docker-compose.host.yaml`: one host of a real platform.
- **Kubernetes** (`deploy/`): Deployment (one replica per node, node-pinned) with
  the app container (MediaMTX built in), ConfigMap, optional GPU, probes,
  ServiceMonitor example, Grafana dashboard. Written so `mxl-poc-platform` can
  vendor it.

---

## 10. Testing

- Unit: config precedence, IS-05 parameter validation, deterministic IDs, channel
  state machine (not routed → waiting → no signal → running → waiting), domain
  resolution including mirror domains, audio pair selection, TSL 5.0 parsing against
  byte vectors of the platform's reference codec (UTF-16 and ASCII, several displays,
  DLE stuffing, with and without DLE/ETX), display mapping and screen filter, the
  tally colour rule, and the tally fields of the API.
- Integration also sends a TSL 5.0 packet over UDP and one over TCP and checks the
  tally fields of channel 1.
- Unit (1.3.0): preview setting precedence and aliases, own and shared mode selection,
  `/widgets`, the widget route's parameters and CSP header, `/statusz` and the preview
  metrics, the child-process supervisor. Integration: the run above uses the built-in
  MediaMTX and checks the widget routes; a second run publishes to a separate MediaMTX
  (shared mode), fetches HLS there under the path prefix, and follows that MediaMTX
  going away (`error`) and coming back (`publishing`).
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
