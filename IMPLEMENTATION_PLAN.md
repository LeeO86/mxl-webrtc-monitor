# mxl-webrtc-monitor — Implementation Plan

This document records how `SPECIFICATION.md` (draft v0.1) is implemented.

## 1. Pins

| Component | Pin |
| --- | --- |
| MXL | `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, built with `-DMXL_ENABLE_FABRICS_OFI=OFF` |
| nmos-cpp | `fe303849527394b03bdedc8f161f377fe458bb62` (same commit as mxl-decklink and mxl-fabrics-agent) |
| GStreamer | 1.24.2 from Ubuntu 24.04 |
| mediamtx | `bluenviron/mediamtx:1.20.1` |
| UI | Vue 3 and hls.js, bundled into one HTML file. No CDN |

GStreamer packages on Ubuntu 24.04: `gstreamer1.0-plugins-base`, `gstreamer1.0-plugins-good`, `gstreamer1.0-plugins-bad`, `gstreamer1.0-plugins-ugly`, `gstreamer1.0-libav`, `gstreamer1.0-rtsp`, `gstreamer1.0-x`, `gstreamer1.0-tools`, plus the matching `-dev` packages and `libgstrtspserver-1.0-dev` at build time. `textoverlay` comes from `gstreamer1.0-x`. `nvcudah264enc` is selected when that element factory reaches `READY`; the Ubuntu package set does not ship a working NVENC plugin without the NVIDIA userspace driver, and the process falls back to x264.

## 2. Deviations

1. **v210 unpack is GStreamer's CPU `v210` raw format** (`video/x-raw,format=v210` into `videoconvert`). There is no separate GPU unpack. `video/v210a` is accepted: the fill plane is pushed as v210 and the alpha plane is ignored.
2. **Default NMOS seed is `HOST_ID-monitor`**, matching the configuration table. The prose also mentions the container name; under host networking `HOST_ID` is the host or pod name. Set `NMOS_SEED` to override it.
3. **IDs are UUIDv5** in the RFC 4122 URL namespace. The name is `mxl-webrtc-monitor/<seed>/node` (and `/device`, `/ch/<n>/video`, `/ch/<n>/audio`).
4. **Audio meters are computed in the reader** (peak and RMS, about 10 Hz), not by the GStreamer `level` element. `METRICS_AUDIO_PEAK` defaults to false and is not in the spec table.
5. **Opus is the only audio codec.** MediaMTX 1.20.1 documents Opus as an HLS codec, so no AAC track is added.
6. **The Query API defaults to `NMOS_REGISTRY_ADDRESS` and `NMOS_REGISTRY_PORT + 1`.** Set `NMOS_QUERY_ADDRESS` and `NMOS_QUERY_PORT` when the Query API is elsewhere. Sender labels and `/readyz` use that API. There is no output domain: `MXL_OUTPUT_DOMAIN_DIR`, `MXL_OUTPUT_DOMAIN_ID` and `history_duration` do not apply, and `MXL_CLEANUP_ON_EXIT=true` does not delete anything. SIGTERM exits 143 after readers are released and the node is removed from the registry.
7. **A missing MXL root does not exit 78.** Exit 78 is invalid configuration. Readiness stays false until the directory exists, NMOS is registered or disabled, and the MediaMTX API answers.
8. **Unrouted audio omits the audio track.** Routed audio that has not produced samples yet pushes digital silence so the player keeps a continuous timeline.
9. **DNS-SD off** sets nmos-cpp `pri` and `highest_pri` to `no_priority`, which disables advertisement and discovery. `NMOS_DNS_SD=true` leaves the nmos-cpp defaults.
10. **Per-channel settings** are `CH<n>_*` environment keys and a `channels` array in the JSON file. They apply at runtime. Global keys changed through the UI set `restart_required`.
11. **x264 uses `ultrafast` and `tune=zerolatency`**, GOP of about one second, no B-frames. NVENC is `nvcudah264enc` with CBR, preset `p1`, tune `ultra-low-latency` and zero reorder delay, fed NV12. The spec names `nvh264enc`; its legacy presets fail on current drivers (deviation 13).
12. **The demo Compose registry is the Python stand-in** in `tests/integration/fake_registry.py`. It implements the registration and query calls this process makes. A facility deployment sets `NMOS_REGISTRY_ADDRESS` to a real registry.
13. **NVENC element is `nvcudah264enc`, not `nvh264enc` (spec §5.5).** With driver 595.84 `nvh264enc preset=low-latency-hq` fails at caps time with "Selected preset not supported", and `nvh264enc` cannot take the P1-P7 presets. `nvcudah264enc` (GStreamer 1.24, same plugin) can. An encoder error after PLAYING also falls back to x264 for that channel, as §5.5 asks for session failures.

## 3. Process

One nmos-cpp node exposes a video receiver and an audio receiver per channel. Each channel has a video reader thread and an audio reader thread. They resolve the domain on every attempt (no negative cache), open the MXL reader, and push into a leaky `appsrc`. A slow encoder drops the oldest buffer and increments `grains_dropped_total{reason="queue_full"}`. Falling out of the ring resynchronises to head minus `READ_OFFSET_GRAINS` and increments `resyncs_total`.

The video thread publishes an H.264 elementary stream. The audio thread publishes Opus when that receiver is enabled. Both go to `rtsp://127.0.0.1:<rtsp>/ch<n>` over TCP. MediaMTX serves WHEP and low-latency HLS.

## 4. Tests

- Unit tests cover config precedence, IS-05 UUID validation, UUIDv5 ids, the channel state machine, domain scan including mirror domains, audio pair selection, backoff, public WHEP/HLS URL parsing, announce-address checks, query-port defaults, tags, config export/import, and IS-05 state reload.
- `tests/integration/monitor.sh` writes a v210 and float32 flow, PATCHes channel 1, waits for `running`, checks that the HLS playlist grows segments, stops the writer and expects `no_signal`, activates a missing flow and expects `waiting`, then creates that flow and expects `running` without another PATCH. It sets `MONITOR_HLS_PUBLIC_URL` and fetches the playlist through the URL the API reports. It then sends SIGTERM and expects exit 143, the node gone from the Query API, and a decoy domain left in place with `MXL_CLEANUP_ON_EXIT=true`.
- Hardware checks in spec §10 (NVENC on A4000 and L4, 4 and 16 channels, browser WebRTC, HLS with UDP blocked) are not run in CI. A lab run on an NVIDIA A16 is in the README ("Hardware check"); A4000, L4 and the browser checks are still open.

## 5. Public WHEP and HLS URLs

`MONITOR_WHEP_PUBLIC_URL` and `MONITOR_HLS_PUBLIC_URL` are optional absolute
origins. When set, `/api/v1/channels` returns
`<origin>/ch<n>/whep` and `<origin>/ch<n>/index.m3u8` and
`playback.public.whep` / `playback.public.hls` is true. The page does not
rewrite a public URL. When a setting is empty, that URL stays
`http://<MONITOR_PUBLIC_IP>:<port>/ch<n>/...` and the page still replaces only
the hostname. An `https:` page shows a tile hint when a playback URL is still
`http:`. ICE and `webrtcAdditionalHosts` stay on `MONITOR_PUBLIC_IP`.

A same-origin proxy of WHEP and HLS through the monitor's own web server is
not implemented.

## 6. Platform guideline G1–G14

| Item | Status | Evidence | Change |
| --- | --- | --- | --- |
| G1 Configuration | met | `src/config/config.cpp:892` env over file over defaults; unknown env ignored at `:911`; invalid values throw `ConfigError` and `src/main.cpp:274` exits 78. Settings table in `README.md`. State under `STATE_DIR` (`src/config/config.cpp:641`, `src/nmos/connections.cpp:61`). No secrets (`src/config/config.cpp:404`). | Added `STATE_DIR`, `SHUTDOWN_TIMEOUT_S`, `MXL_CLEANUP_ON_EXIT`, and the NMOS keys below. |
| G2 MXL domains | N/A | This process only reads domains (`src/domain/scan.cpp:26`, mirrors at `:53`). It never creates `domain_def.json`. `MXL_OUTPUT_DOMAIN_DIR` and `MXL_OUTPUT_DOMAIN_ID` are unknown env vars and are ignored. `history_duration` does not apply. `MXL_CLEANUP_ON_EXIT=true` logs and deletes nothing (`src/main.cpp:264`). | No output domain is created. |
| G3 NMOS identity | met | UUIDv5 node, device and receivers from `NMOS_SEED` (`src/nmos/ids.cpp:20`). No sources, flows, senders or output domain id. `NMOS_LABEL` (`src/config/config.cpp:371`, `src/nmos/node.cpp:231`). `NMOS_TAGS` on node and device (`src/nmos/node.cpp:179`). Group hints stay on receivers. | Added `NMOS_LABEL` and `NMOS_TAGS`. |
| G4 Registry, no DNS-SD | met | `NMOS_REGISTRY_ADDRESS` / `NMOS_REGISTRY_PORT`. Query defaults (`src/config/config.hpp:73`). `NMOS_DNS_SD=false` sets `pri` and `highest_pri` to `INT_MAX` (`src/nmos/node.cpp:241`). The integration test runs without Avahi. | Added `NMOS_QUERY_ADDRESS` and `NMOS_QUERY_PORT`. |
| G5 Announce IP addresses | met | `NMOS_HOST_ADDRESS` defaults to `MONITOR_PUBLIC_IP` (`src/config/config.cpp:633`). `host_address` and `host_addresses` (`src/nmos/node.cpp:237`). ICE stays on `MONITOR_PUBLIC_IP` (`src/ops/mediamtx.cpp:56`). Playback copy uses that IP unless a public URL is set (`src/ops/api.cpp:33`). Loopback and names exit 78 (`src/config/config.cpp:349`). `MONITOR_WHEP_PUBLIC_URL` and `MONITOR_HLS_PUBLIC_URL` may be TLS names. | `NMOS_HOST_ADDRESS` added. `MONITOR_PUBLIC_IP` remains the ICE alias. `127.0.0.1` is no longer a valid announce address. |
| G6 Ports | met | Web, NMOS (`NMOS_PORT`, WebSocket `NMOS_PORT+1`), RTSP, API, WHEP, HLS, ICE and `MEDIAMTX_METRICS_PORT` (`src/ops/mediamtx.cpp:29`). Bind failure of the web or NMOS port exits 75 (`src/main.cpp:224`). MediaMTX is a separate process; until it answers, `/readyz` is 503. | Added `MEDIAMTX_METRICS_PORT`. |
| G7 Health and metrics | met | `/livez` (`src/ops/api.cpp:161`). `/readyz` requires the MXL root, MediaMTX, and Query API registration when a registry is set (`src/ops/api.cpp:165`, `src/nmos/node.cpp:103`). `/metrics` prefix `mxl_webrtc_monitor_` (`src/ops/metrics.cpp:59`). | Readiness uses `NMOS_QUERY_*` and requires the node to be registered. |
| G8 Clean shutdown | met | SIGTERM: stop media, skip domain removal, erase the node so nmos-cpp sends DELETE, exit 143 (`src/main.cpp:260`, `src/nmos/node.cpp:460`). Budget `SHUTDOWN_TIMEOUT_S` (default 10). `MXL_CLEANUP_ON_EXIT` does not delete another domain. | Exit 143 on SIGTERM. Registry DELETE. No domain to remove. |
| G9 IS-05 | met | Receivers accept staged `sender_id`, `master_enable`, `transport_params` and `activate_immediate`. `master_enable: false` stops the reader. Active routes persist in `STATE_DIR/is05.json` (`src/nmos/connections.cpp:61`). Senders reporting `mxl_domain_id` / `mxl_flow_id` are N/A: this process has no senders. | Added IS-05 persistence. |
| G10 Config export and import | met | `GET /api/v1/config/export` and `POST /api/v1/config/import` (`src/ops/api.cpp:216`). `secrets_included: false`. Existing `/api/v1/config` and `/api/v1/config.env` remain. No presets or layouts. | Added the two endpoints. |
| G11 Image and CI | met | `.github/workflows/container.yaml:29` publishes `X.Y.Z`, `X.Y`, `X` on a version tag, and `git-<sha7>` plus `nightly-dev` on `main`. No moving `latest` tag. Runtime uid 1000 (`docker/Dockerfile:78`). OCI labels include `io.dmf.mxl.revision` (`docker/Dockerfile:81`, `container.yaml:35`). The example manifest uses `ghcr.io/leeo86/mxl-webrtc-monitor:1.0.0`. | Removed the `latest` tag. Pinned the MXL revision label. |
| G12 Kubernetes example | met | `deploy/mxl-webrtc-monitor.yaml:43` host network (ICE), standard env, `/readyz` and `/livez`, `terminationGracePeriodSeconds: 30`, MXL root hostPath (`:128`), writable `/config` (`:132`), uid 1000, no `hostIPC`, no extra capabilities. | Grace period, `STATE_DIR`, seed, label, tags, host IP. |
| G13 Documentation | met | `README.md` settings, ports, exit codes, API and platform section. `CHANGELOG.md` 1.0.0. `SPECIFICATION.md` matches the code. | Updated for v1.0.0. |
| G14 Tests | met | `tests/unit/test_platform.cpp` and `tests/unit/test_config.cpp`. `tests/integration/monitor.sh` covers start, ready, SIGTERM, exit 143, node deregistered, decoy domain kept. | Added the platform cases and the shutdown checks. |
