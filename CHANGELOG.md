# Changelog

## 1.4.1

Interlaced inputs. No setting changes are needed; progressive inputs are read as before.

### Fixes

- **Interlaced flows were shown squashed and the preview stalled.** MXL (218ddaa) stores an interlaced flow as one field per grain: half the frame's lines, at twice the definition's `grain_rate` (a 1080i25 flow has 50 grains per second). 1.4.0 took each field for a frame: the field filled the upper half of the picture (black below), and 50 pictures per second went to the encoder with 25 fps timestamps, so the RTSP publish waited longer and longer (lab, 1080i25 from mxl-test-player 1.2.1: the encoder at 2 fps after a few seconds, then 0; the page froze). Now the monitor reads both fields of a frame (grain 2k is the first field: top for `interlaced_tff`, bottom for `interlaced_bff`) and encodes one picture per frame at the frame rate. Lab, same source: 50 grains and 25 encoded frames per second, the browser plays 25.0 fps over WebRTC with no dropped frames, the whole picture.
- **The preview of an interlaced frame** is its first field alone when the preview is not taller than a field (the default 540 lines from 1080i: every line it can show, no combing; the same bytes as 1.4.0's conversion of a whole tff frame), and the two fields woven when it is taller. `field_preview` in each channel says which (`first_field` or `weave`), `interlace_mode` the flow's field order; the Channels tab shows both.
- **Every grain is checked against the flow definition before it is read.** Grains smaller than the definition's field or frame (`grain_too_small`, e.g. a `flow_def.json` that says progressive for a field flow), an interlaced flow with an odd height (`odd_interlaced_height`), no raster (`no_raster`) or an interlaced definition whose grains MXL does not keep as fields (`format_mismatch`: MXL's own grain rate is not twice the definition's, e.g. whole-frame grains under a `flow_def.json` that now says interlaced; they passed the size check and would be shown squashed) put the video leg in the new state `unsupported` with that reason: a slate, one `video_unsupported` log line and one count in the new metric `mxl_webrtc_monitor_video_unsupported_total{channel,reason}` per flow, format and reason, never a read past the grain. The monitor opens the flow again every 200 ms and shows it once a writer re-creates it with a fitting definition. `mxl_webrtc_monitor_channel_state` has the state `unsupported`; the page shows it in red with the reason and a header pill. A progressive grain smaller than its frame was padded with black before; it is unsupported now.
- With the first-field preview only the shown field (grain 2k) has to be there and valid: a missing or invalid second field does not turn the frame into a slate. The woven preview needs both.
- **A video read that stays too late no longer spins a core** (also in 1.4.0). After a too-late (or other failed) read the reader retried at once, without updating the state or publishing anything. With `READ_OFFSET_GRAINS` beyond the flow's ring (MXL keeps 200 ms by default: 5 grains at 25p, 10 fields at 1080i25), every read is too late: one core at 100 %, the channel left in its last state (`waiting` right after a route) and no stream published (lab, 1080p25 with `READ_OFFSET_GRAINS=10`). Now a failed read updates the state (`no_signal` with a slate after 500 ms without a good grain) and a too-late retry waits one grain (lab, same case: 23 retries per second, 22 % CPU with the slate encode, `no_signal`, publishing).
- For an interlaced flow `READ_OFFSET_GRAINS` counts frames (the delay in time stays that of a progressive flow of the same frame rate); `grains_read_total` and `read_lag_grains` count grains (fields).
- `mxl-flow-writer` (test tool) writes interlaced flows (`tff` or `bff` as the ninth argument). `tests/integration/interlaced.sh` (CI) decodes the tff and bff previews and checks the rates, the first field, the woven field order and the unsupported flows (`grain_too_small` and `format_mismatch`), and a flow read too late every time (`no_signal`, a slate published, at most one retry per field).
- The Compose and Kubernetes examples use the `1.4.1` image.

## 1.4.0

The preview conversion with AVX2 (plan G). No setting changes are needed; the output is byte for byte the same.

- **AVX2 preview conversion.** `v210ToPreview` (v210 to the encoder's 4:2:0 picture) has an AVX2 version. The image stays plain x86-64 and picks it at start when the CPU has AVX2; otherwise the scalar code runs. Both give the same bytes. Lab Xeon Gold 6136, 1080p50 to 960×540: 3.2–3.5 → 1.2 ms per frame, about 0.1 cores less per channel (README, "Hardware check").
- `MONITOR_SIMD` (`auto`, `avx2` or `scalar`; default `auto`, environment only) overrides the choice. The start logs it (`"event":"simd"`), `/api/v1/info` has `simd`, and the new metric `mxl_webrtc_monitor_simd_level{level}` is 1.
- `mwm-preview-bench` (built with the unit tests) times the scalar and the AVX2 conversion; CI prints the runner's numbers.
- The Compose and Kubernetes examples use the `1.4.0` image.

## 1.3.0

The platform's shared-preview contract (platform spec §11.5, D-185) and the first operator-screen widget. Backwards compatible: the 1.2.0 setting names stay as aliases.

- **MediaMTX is built in.** The image carries the official MediaMTX 1.20.1 binary (`/usr/local/bin/mediamtx`, MIT licence in `/usr/share/doc/mediamtx/`, about 55 MB). Without `PREVIEW_PUBLISH_URL` (own mode, the default) the monitor starts it as a child process with the generated config, starts it again when it exits (1 s, doubling to 10 s) and stops it on shutdown. No sidecar container is needed. Its RTSP ingest is `127.0.0.1:MEDIAMTX_RTSP_PORT` (new, default 8554); the other `MEDIAMTX_*` ports are unchanged.
- **Shared mode.** `PREVIEW_PUBLISH_URL` (e.g. `rtsp://mxl-mediamtx.mxl-platform.svc:8554`) publishes the channels to that MediaMTX and starts none here. `/readyz` then does not depend on MediaMTX; `MEDIAMTX_API_URL` is used only when set (viewers, stream state).
- **Stream paths** are `<PREVIEW_PATH_PREFIX>/ch<n>` (default prefix `mxl-webrtc-monitor`, e.g. `test-all/mon/ch1` with `test-all/mon`). The page and the API's `playback` URLs follow; external links to `/ch<n>/...` need the prefix.
- **Setting names.** `PREVIEW_PUBLISH_URL`, `PREVIEW_PATH_PREFIX`, `PREVIEW_WHEP_URL` and `PREVIEW_HLS_URL`, the same in every repo with browser previews. `MEDIAMTX_RTSP_URL`, `MONITOR_WHEP_PUBLIC_URL` and `MONITOR_HLS_PUBLIC_URL` stay accepted as aliases; the new name wins when both are set. A 1.2.0 sidecar deployment sets `MEDIAMTX_RTSP_URL`, so it runs in shared mode against its sidecar, which still gets its config. A sidecar deployment without `MEDIAMTX_RTSP_URL` should drop the sidecar: the built-in MediaMTX takes the same ports.
- **Publish state.** Each channel reports `connecting`, `publishing` or `error` (with the error) for its RTSP publish: `preview` in `/api/v1/channels` and the events, `preview` in `/statusz` (mode, publish URL, prefix, the built-in MediaMTX's `running` and `restarts`, the streams), and the metrics `mxl_webrtc_monitor_preview_mode{mode}` and `mxl_webrtc_monitor_preview_publish_state{channel,state}`.
- **Widgets.** `GET /widgets` lists the operator-screen widgets; `GET /widget/channel?ch=<n>[&labels=][&meters=][&theme=dark|light|transparent]` is one channel's tile without the app around it (picture, label, TSL lamps, tally border and text tally, meters), using the monitor's own API. The `/widget` routes send `Content-Security-Policy: frame-ancestors <WIDGET_FRAME_ANCESTORS>` (new, default `'self'`) and no `X-Frame-Options`. The widget posts `widget-ready` and `widget-size` to its parent.
- Web UI: the Status tab shows the preview mode, publish URL, path prefix, the built-in MediaMTX and each channel's publish state; the Channels tab the stream path and publish state; the header a pill for channels that are not published. The Settings tab lists the new names.
- The Compose and Kubernetes examples use the `1.3.0` image and no MediaMTX sidecar.

## 1.2.0

TSL UMD 5.0 tally per channel (platform request T3). Off by default, so existing deployments are unchanged.

- Tally receiver, the same as mxl-multiviewer 1.3.0's: `TSL_ENABLE=true` listens on `TSL_UDP_PORT` (default 8912) and `TSL_TCP_PORT` (default 8913, DLE/STX framing; DLE/ETX is accepted, not required). UTF-16LE labels (surrogate pairs included) become UTF-8; screen control data and control data messages are skipped. With an empty `TSL_MAP`, display index `i` is channel `i+1` (0-based, as in the multiviewer); `TSL_MAP` (`display:channel` pairs) maps them otherwise, and `TSL_SCREEN` (default -1: every screen) accepts one screen only. A TSL port that cannot be bound exits 75; an invalid `TSL_MAP` or `TSL_SCREEN` exits 78.
- The defaults are not the multiviewer's 8910/8911: both run with host networking, and a monitor and a multiviewer on one host would otherwise collide.
- API (backwards compatible): each channel in `GET /api/v1/channels` and on the WebSocket has `tsl_lh`, `tsl_rh`, `tsl_text_tally` (0 off, 1 red, 2 green, 3 amber), `tally` (the border colour: text tally, else RH, else LH) and `tsl_text`, the multiviewer's field names. The new channel setting `tally_text` (`CH<n>_TALLY_TEXT`, default false) is in the status and taken by `PATCH /api/v1/channels/{n}`.
- Web UI: with TSL on, each Multiview tile has a left lamp (LH) and a right lamp (RH) at the ends of its title (an off lamp is not drawn) and a border around the picture in the `tally` colour. With `tally_text` the channel label sits on the text tally colour. The Channels tab has the `tally_text` switch and the channel's tally fields; the Settings tab groups the `TSL_*` keys. The tally is not burned into the stream.
- The Compose and Kubernetes examples use the `1.2.0` image.

## 1.1.0

- New web UI in the look of the other LeeO86 media functions (mxl-test-player, mxl-replay, mxl-multiviewer, mxl-st2110-gateway, mxl-browser-source): header with the node label, running, waiting, no-signal, MediaMTX, registration and connection pills and the versions; banners for a lost API, lost live updates, a needed restart, playback blocked on https and failed actions; tabs in the URL hash; light and dark theme. Every API function has a control:
  - **Multiview**: every channel's player (WHEP, HLS fallback) in a grid (automatic, 1×1 to 4×4, kept in the browser), full size on a click. Per tile the video state, source, format or the reason it waits, audio state and inputs, encoder, preview height, viewers, the protocol that plays (WebRTC or HLS), audio meters with the monitored pair outlined, and Unmute/Mute.
  - **Channels**: the selected channel's settings as a draft with Apply and Revert: video and audio labels, preview height, max frame rate, video bitrate, audio pair (offered from the input channels), downmix, audio bitrate, and the overlay with its parts (channel label, source label, format; the parts are new in the UI). Settings from the environment are read-only and marked ENV. Next to it the channel's IS-05 routes (state, reason, receiver, `master_enable`, sender, domain, flow, source label) and its stream (format, encoder, viewers, MediaMTX path and tracks, WHEP and HLS URLs to copy or open).
  - **NMOS**: node, device, address, registry and Query API, DNS-SD, seed, links to the raw IS-04 and IS-05 resources, and every receiver with `master_enable`, state, reason, sender, domain, flow and its IS-05 `active` resource. Routing stays IS-05 only.
  - **Status**: `/livez`, `/readyz` with its parts, restart required, versions (MXL, nmos-cpp, GStreamer, running and pinned MediaMTX, encoders), MediaMTX (streams ready, API, ingest, WHEP, HLS, ICE, metrics port), and per channel the `/metrics` counters (encode fps and mean latency, output bitrate, grains read, drops, resyncs, lag, encoder fallbacks) with the viewers and the MediaMTX path.
  - **Settings**: every setting with its value and origin, in groups. Settings that the environment does not set are edited and saved to `MONITOR_CONFIG_FILE` (*Default* removes a file value; RESTART marks what applies at start). Export as JSON or `KEY=value` (copy, download); import from a file or pasted text.
  - Edits are drafts in the page: tab switches, status pushes and WebSocket reconnects keep them. The multiview stays mounted, so switching tabs does not restart its players.
- The player keeps the 1.0.5 rule: it starts again only when its URLs, video state, video flow or audio routing change. A WebRTC session that played and then drops (the channel's stream was rebuilt after a settings change, MediaMTX restarted) now starts again after 1.5 s instead of switching to HLS for good; HLS stays the fallback when WebRTC never shows a picture. A failed HLS player starts again too. Unmute stays on when the player starts again.
- API additions (all backwards compatible): `GET /api/v1/info` has `label` (the node label). Each channel in `GET /api/v1/channels` and on the WebSocket has `overlay_label`, `overlay_source` and `overlay_format` (which `PATCH /api/v1/channels/{n}` already took) and `mediamtx` with `ready` and `tracks` of its MediaMTX path (from `GET /v3/paths/list`, polled once a second with the viewers).

### Fixes

- A rejected change (`PATCH /api/v1/channels/{n}` or `PUT /api/v1/config` with an invalid value) stayed in the process's copy of the configuration file, so every later change failed with the same error until a restart; a rejected `PUT` also set `restart_required`. A rejected change now leaves everything as it was.
- Channel settings saved in `MONITOR_CONFIG_FILE` (its `channels` list, which the monitor writes on every channel change) are read at start and by `PUT /api/v1/config`. They were skipped, so a restart lost them, and the next saved change wrote the file without them.
- `GET /api/v1/nmos` escapes the receiver labels. A label with a quote or a backslash made the answer invalid JSON (the NMOS page stayed empty).
- The 1.0.5 Settings page saved every effective setting with `PUT /api/v1/config`. The API refuses that as soon as one key comes from the environment (always true in a deployment), and it would have written every default into the file. The page sends the file's own keys plus the edits.
- Viewer counts work with more than one channel. The monitor's HTTP client did not decode chunked answers, and MediaMTX sends `GET /v3/paths/list` chunked once it is longer than 2 KiB (two or more paths), so `viewers` stayed 0 (lab: 4 channels). A channel whose MediaMTX path is gone reports 0 viewers; the counts kept their last value.
- The generated `mediamtx.yml` sets `rtspTransports: [tcp]` (the channels publish over TCP). MediaMTX bound its RTSP UDP ports 8000 and 8001 in every instance, so a second monitor's MediaMTX on the same host failed with "address already in use" although all the documented ports were distinct.
- The Compose and Kubernetes examples use the `1.1.0` image.

## 1.0.5

- Audio no longer stalls the stream. Audio timestamps count pushed samples, and 1.0.4 pushed one video frame of audio (960 samples at 50p) each time the audio head moved. A fabrics mirror moves it 480 samples every 10 ms, so the audio ran at twice real time; the live RTSP sink then held the audio branch, every audio push hit a full queue (`grains_dropped_total{reason="queue_full"}`), and MediaMTX got an Opus track without samples. Chrome showed a black picture for HLS and WebRTC although the video was fine (platform: test-all-mon with a mirrored mxl-multiviewer audio output; reproduced on the lab with a fabrics mirror). Now the stream carries exactly as much audio as the pushed video frames cover, read sample by sample after the last pushed sample. Samples that do not arrive within a frame, or a flow that is not readable (not found, no signal, out of the ring), become silence, so the Opus track always has data.
- Audio that arrives later than the video (a mirror next to local video) is read at its head instead of at the video grain's time; 1.0.4 asked for the aligned samples, got `too late` and retried without pause (`resyncs_total` grew by about 700/s, one core busy). A position more than three frames from the target is placed again and counted in `resyncs_total`. An audio flow counts as live while its head moved within 500 ms (100 ms before, shorter than some writers' batches).
- Web UI: a tile's player starts again only when its playback URLs, video state, video flow or audio routing change. The `<video>` ref was an inline function, which Vue calls on every render, and the status arrives about 10 times a second, so every tile tore down and renegotiated its WHEP/HLS player every 100 ms and never showed a picture. Status updates are merged into the existing channel objects. The WHEP-to-HLS fallback detaches the WebRTC stream first (HLS never played while `srcObject` was set).
- Web UI: the Channels tab edits a copy of the settings (the status pushes overwrote what was typed), and "Copy KEY=value" works (it called `navigator` from the template).
- `mediamtx.yml` is written to a temporary file and renamed over the old one, and not rewritten when unchanged. MediaMTX reloads on every change of the file and read it half written on the lab: it ran on its defaults (no API, "path is not configured").
- The Compose and Kubernetes examples use the `1.0.5` image.

## 1.0.4

- Audio is read. MXL's continuous flow writer never sets `lastWriteTime` (only the discrete writer does), and the audio leg took a flow as live only when `lastWriteTime` was less than 100 ms old, so every audio input stayed `no_signal` with `audio_channels` 0 and no meters, whatever wrote it (seen on the platform with mxl-multiviewer, mxl-replay and mxl-test-player audio, local and mirrored). An audio flow is now live while its head index moves (within 100 ms). The integration test checks the audio state as well.

## 1.0.3

Less CPU per channel; same pictures, settings and API. Lab A16 host (README), 1080p50 sources, 540p preview: 16 NVENC channels 6.4 → 4.1 cores, 8 x264 channels 4.2 → 3.2 cores, 16 x264 channels 8.9 → 6.3 cores, encode latency 3 → 1.5 ms.

- The overlay text is passed to `textoverlay` only when it changes. The video loop set it on every grain, and `textoverlay` lays out and renders its text again on every set (Pango and Cairo were about 10 % of the process).
- The preview conversion (`v210ToPreview`) sums whole source lines word by word, takes each output pixel's sum from prefix sums, and divides by a precomputed reciprocal instead of the `div` instruction. The output bytes are the same (unit test against the old code); a 1080p frame into 640×360 takes 2.0 instead of 3.6 ms on the lab CPU.

## 1.0.2

- `GET /api/v1/info` reports the running MediaMTX in `mediamtx` (from its `GET /v3/info`, `unknown` when it does not answer) and the version of the examples in the new `mediamtx_pin`. It always said `1.20.1`, the pin, also next to another sidecar.

## 1.0.1

- The encoder's picture is made straight from the MXL grain: one pass over the v210 frame averages it to the preview size, 8-bit 4:2:0 (NV12 for NVENC, I420 for x264), and only that small picture enters GStreamer. Before, every full-size 10-bit frame was copied into GStreamer and converted and scaled there by `videoconvert` and `videoscale`. Interlaced sources use their first field, as `deinterlace method=bob` did. Lab A16 host (README): 16 NVENC channels 10.4 → 6.4 cores, 8 x264 channels 6.2 → 4.2 cores, encode latency 13 → 3 ms.
- v210 rows are padded to 128 bytes (48 pixels) as MXL writes them. Widths that are not a multiple of 48, such as 1280 (720p), were read with a 3424-byte instead of a 3456-byte stride.
- NVENC uses `nvcudah264enc` (preset `p1`, tune `ultra-low-latency`, CBR, zero reorder delay) with NV12 input. The legacy `nvh264enc` presets fail on current NVIDIA drivers with "Selected preset not supported" (seen on driver 595.84), and every channel retried NVENC forever.
- An encoder error after the pipeline is playing now switches that channel to x264 and increments `mxl_webrtc_monitor_encoder_fallbacks_total`, the same as a failure while building the pipeline.
- The Compose files and `deploy/mxl-webrtc-monitor.yaml` start MediaMTX without `sh`. The MediaMTX image has no shell, so the sidecar never started.

## 1.0.0

Stable platform contract. A later breaking change needs 2.0.0.

- `NMOS_HOST_ADDRESS` is the IP announced on the IS-04 node href, API endpoints and IS-05 control hrefs. It defaults to `MONITOR_PUBLIC_IP`. `MONITOR_PUBLIC_IP` remains the ICE address (`webrtcAdditionalHosts`). Both must be IP literals. Loopback, `0.0.0.0` and hostnames are rejected (exit 78). An empty `MONITOR_PUBLIC_IP` still means the first non-loopback IPv4.
- `NMOS_QUERY_ADDRESS` defaults to `NMOS_REGISTRY_ADDRESS`. `NMOS_QUERY_PORT` defaults to the registration port + 1. Readiness and sender labels use that Query API.
- `NMOS_LABEL` sets the node label and the device label prefix. `NMOS_TAGS` (JSON object of string arrays) is added to the node and device. Receiver group hints are unchanged. `NMOS_SEED` still derives the node, device and receiver ids.
- `NMOS_DNS_SD=false` (the default) keeps DNS-SD browsing and mDNS advertisement off.
- `STATE_DIR` (default `/config`) holds `is05.json` and, unless `MEDIAMTX_CONFIG_PATH` is set, `mediamtx.yml`. Active IS-05 routes are restored from `is05.json` after a restart.
- `SHUTDOWN_TIMEOUT_S` (default 10). SIGTERM and SIGINT stop media, release MXL readers, erase the NMOS node so the registry receives DELETE, and exit 143.
- `MXL_CLEANUP_ON_EXIT` is accepted and defaults to false. This monitor owns no output domain, so `true` does not delete a directory. `MXL_OUTPUT_DOMAIN_DIR` and `MXL_OUTPUT_DOMAIN_ID` stay unknown environment variables and are ignored.
- `MEDIAMTX_METRICS_PORT` (default: MediaMTX API port + 1) is written into the generated sidecar config.
- `GET /api/v1/config/export` and `POST /api/v1/config/import`. The document has no secrets (`secrets_included: false`). Import skips keys set by the environment. Existing `/api/v1/config` and `/api/v1/config.env` stay.
- The container image no longer publishes a moving `latest` tag. `vX.Y.Z` still publishes `X.Y.Z`, `X.Y` and `X`. `main` still publishes `git-<sha7>` and `nightly-dev`.
- `MONITOR_WHEP_PUBLIC_URL` and `MONITOR_HLS_PUBLIC_URL` are unchanged and may still be TLS hostnames.
