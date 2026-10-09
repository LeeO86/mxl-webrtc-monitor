# Changelog

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
