# mxl-webrtc-monitor

Watch and listen to MXL flows in a browser. Each monitor channel is a pair of NMOS BCP-007-03 MXL receivers (video and audio). A controller routes any sender to a channel with IS-05. The process reads the local MXL domain, encodes a low-bitrate preview, and publishes it to a [MediaMTX](https://github.com/bluenviron/mediamtx) sidecar. The built-in page shows every channel, with labels, state and audio meters.

`mxl-fabrics-agent` sees the NMOS subscription and can replicate the flow onto this host. This process does not know about hosts or RDMA.

## Ports

Every listening port is an environment variable. Two monitors on one host need distinct values.

| Port | Setting | Use |
| --- | --- | --- |
| 8100 | `WEB_PORT` | UI, REST, `/livez`, `/readyz`, `/metrics` |
| 3242 | `NMOS_PORT` | NMOS Node API |
| 3243 | `NMOS_PORT` + 1 | NMOS Node WebSocket |
| 8554 | `MEDIAMTX_RTSP_URL` | RTSP ingest on localhost (MediaMTX) |
| 8889 | `MEDIAMTX_WHEP_PORT` | WHEP |
| 8888 | `MEDIAMTX_HLS_PORT` | Low-latency HLS |
| 8189 | `MEDIAMTX_ICE_UDP_PORT` | ICE, one UDP port and a TCP fallback on the same port |
| 9997 | `MEDIAMTX_API_URL` | MediaMTX API, localhost only |
| 9998 | `MEDIAMTX_METRICS_PORT` | MediaMTX metrics. Default is the API port + 1 |

These defaults do not collide with mxl-decklink (8080, 3212), mxl-st2110-gateway (8090), mxl-fabrics-agent (8095, 3232) or FlowXer (9620). If `WEB_PORT` or `NMOS_PORT` cannot be bound, the process exits 75. MediaMTX binds its own ports; until its API answers, `/readyz` stays 503.

## Settings

Environment, then `MONITOR_CONFIG_FILE`, then the defaults below. Unknown environment variables are ignored. Invalid values exit 78. There are no secrets.

| Key | Default | Meaning |
| --- | --- | --- |
| `HOST_ID` | hostname | seed and label fallback. Not an announced address |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | parent of input domains, including `mirror-*` |
| `MONITOR_CHANNELS` | 4 | 1–16 |
| `MONITOR_PREVIEW_HEIGHT` | 540 | preview height |
| `MONITOR_MAX_FPS` | 0 | 0 = source rate |
| `MONITOR_VIDEO_BITRATE_KBPS` | 2000 | per channel |
| `MONITOR_AUDIO_BITRATE_KBPS` | 128 | per channel |
| `READ_OFFSET_GRAINS` | 2 | read behind head |
| `ENCODER` | `auto` | `auto`, `nvenc`, `x264` |
| `MONITOR_PUBLIC_IP` | first non-loopback IPv4 | ICE host. Alias source for `NMOS_HOST_ADDRESS` |
| `NMOS_HOST_ADDRESS` | `MONITOR_PUBLIC_IP` | IP on the IS-04 href, API endpoints and IS-05 control hrefs |
| `MONITOR_WHEP_PUBLIC_URL` | empty | browser WHEP origin. A TLS hostname is allowed |
| `MONITOR_HLS_PUBLIC_URL` | empty | browser HLS origin. A TLS hostname is allowed |
| `STATE_DIR` | `/config` | `is05.json` and, by default, `mediamtx.yml` |
| `SHUTDOWN_TIMEOUT_S` | 10 | SIGTERM budget |
| `MXL_CLEANUP_ON_EXIT` | false | accepted; this monitor has no output domain to remove |
| `NMOS_LABEL` | empty | node label and device label prefix |
| `NMOS_TAGS` | `{}` | JSON object of tag name to string array, on the node and device |
| `MEDIAMTX_RTSP_URL` | `rtsp://127.0.0.1:8554` | sidecar ingest |
| `MEDIAMTX_API_URL` | `http://127.0.0.1:9997` | sidecar API |
| `MEDIAMTX_CONFIG_PATH` | `$STATE_DIR/mediamtx.yml` | generated sidecar config |
| `MEDIAMTX_WHEP_PORT` | 8889 | WHEP |
| `MEDIAMTX_HLS_PORT` | 8888 | HLS |
| `MEDIAMTX_ICE_UDP_PORT` | 8189 | ICE UDP and TCP |
| `MEDIAMTX_METRICS_PORT` | API port + 1 | sidecar metrics. `0` selects the default |
| `NMOS_ENABLE` | true | |
| `NMOS_REGISTRY_ADDRESS` | empty | registration API host |
| `NMOS_REGISTRY_PORT` | 3210 | registration API port |
| `NMOS_QUERY_ADDRESS` | registry address | Query API host |
| `NMOS_QUERY_PORT` | registry port + 1 | Query API port |
| `NMOS_DNS_SD` | false | DNS-SD browse and mDNS advertisement |
| `NMOS_PORT` | 3242 | Node API. WebSocket is the next port |
| `NMOS_SEED` | `$HOST_ID-monitor` | UUIDv5 seed for node, device and receivers |
| `WEB_PORT` | 8100 | UI, REST, health, metrics |
| `LOG_LEVEL` | `info` | |
| `MONITOR_CONFIG_FILE` | empty | optional JSON file under the environment |
| `METRICS_AUDIO_PEAK` | false | publish per-channel peak gauges |

`MONITOR_PUBLIC_IP` stays the ICE address. `NMOS_HOST_ADDRESS` is the NMOS address and defaults to it. Playback URLs use `MONITOR_PUBLIC_IP` unless the public WHEP or HLS origin is set. `MXL_OUTPUT_DOMAIN_DIR` and `MXL_OUTPUT_DOMAIN_ID` are ignored: this process only reads domains.

Channel keys are `CH<n>_VIDEO_LABEL`, `_AUDIO_LABEL`, `_PREVIEW_HEIGHT`, `_VIDEO_BITRATE_KBPS`, `_AUDIO_BITRATE_KBPS`, `_MAX_FPS`, `_AUDIO_PAIR`, `_DOWNMIX`, `_OVERLAY`, `_OVERLAY_LABEL`, `_OVERLAY_SOURCE`, `_OVERLAY_FORMAT`.

## HTTP API

| Method | Path |
| --- | --- |
| GET | `/`, `/livez`, `/readyz`, `/statusz`, `/metrics` |
| GET | `/api/v1/info`, `/api/v1/channels`, `/api/v1/nmos` |
| PATCH | `/api/v1/channels/{n}` |
| GET | `/api/v1/events` (WebSocket) |
| GET, PUT | `/api/v1/config` |
| GET | `/api/v1/config.env` |
| GET | `/api/v1/config/export` |
| POST | `/api/v1/config/import` |

`GET /api/v1/info` has the version, `label` (the node label), the MXL, nmos-cpp, GStreamer and MediaMTX versions and the available encoders. Each channel in `GET /api/v1/channels` (and on the WebSocket) has its settings, both IS-05 legs (`state`, `reason`, `master_enable`, domain, flow, sender), format, encoder, viewers, `mediamtx` (`ready` and `tracks` of its MediaMTX path), playback URLs and meters (peak and RMS per input channel). `PATCH /api/v1/channels/{n}` takes the channel settings (`video_label`, `audio_label`, `preview_height`, `video_bitrate_kbps`, `audio_bitrate_kbps`, `max_fps`, `audio_pair`, `downmix`, `overlay`, `overlay_label`, `overlay_source`, `overlay_format`). `PUT /api/v1/config` replaces the configuration file with the given keys. A rejected change leaves the settings as they were.

`/readyz` is 200 only when the MXL root is a directory, MediaMTX answers, and, when `NMOS_REGISTRY_ADDRESS` is set, the Query API returns this node. Export is one JSON document with `version`, `settings` and `secrets_included: false`. Import restores settings and channels into `MONITOR_CONFIG_FILE` and skips keys set by the environment. There is nothing secret to omit.

## Web UI

`http://<node>:<WEB_PORT>/` uses only this API. The tabs keep their place in the URL (`#channels`); the page follows the browser's light or dark theme.

- **Multiview**: every channel's player in a grid (automatic or 1×1 to 4×4); a click shows one channel full size. Each tile shows the state, source, format, audio, encoder, viewers, whether WebRTC or HLS plays, and the audio meters with the monitored pair outlined. Audio is muted until you unmute a tile.
- **Channels**: one channel's settings (labels, preview size, frame rate, bitrates, audio pair, downmix, overlay), applied at once; its IS-05 routes and its stream (MediaMTX path, WHEP and HLS URLs).
- **NMOS**: node, registration and every receiver's active IS-05 parameters. There is no source picker: routing is IS-05 only.
- **Status**: health probes, versions, MediaMTX, and per channel the counters of `/metrics`.
- **Settings**: every setting with its origin (ENV, FILE, DEFAULT), editing of the non-environment settings into `MONITOR_CONFIG_FILE`, export and import.

Edits stay in the page until they are applied, also across tab switches and reconnects.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | not used for SIGTERM |
| 75 | `WEB_PORT` or `NMOS_PORT` could not be bound, or startup failed |
| 78 | invalid configuration |
| 143 | SIGTERM or SIGINT finished shutdown, or `SHUTDOWN_TIMEOUT_S` elapsed |

Shutdown stops the media threads and releases MXL readers, erases the node from the NMOS model so the registry is sent DELETE, and exits 143. `MXL_CLEANUP_ON_EXIT=true` does not remove a directory.

## Platform

The monitor uses the host network because browsers send ICE to `MONITOR_PUBLIC_IP` (the node IP). Set `NMOS_HOST_ADDRESS` to that same IP. Set `NMOS_SEED` to `<production>-<function>`, `NMOS_LABEL`, and `NMOS_TAGS` for the production and function. Point `NMOS_REGISTRY_ADDRESS` at the platform registry and leave DNS-SD off. Query defaults to the registration port plus one. Set `MONITOR_WHEP_PUBLIC_URL` and `MONITOR_HLS_PUBLIC_URL` to the ingress origins. Mount `/Volumes/mxl` read-only and a writable `/config` at `STATE_DIR`. `terminationGracePeriodSeconds` must be greater than `SHUTDOWN_TIMEOUT_S`. The process runs as uid 1000 and does not need extra capabilities or `hostIPC`. It does not create an MXL domain. `deploy/mxl-webrtc-monitor.yaml` is an example.

WebRTC needs UDP from the browser to the ICE port. If UDP is blocked, the page falls back to HLS, which is ordinary HTTP.

## HTTPS reverse proxy

When the UI is opened through an HTTPS proxy, set the public origins of WHEP and HLS. The page uses those URLs as given. ICE still uses `MONITOR_PUBLIC_IP` and does not go through the proxy.

```bash
MONITOR_WHEP_PUBLIC_URL=https://mon1-whep.small.mxl.ipla.media.int
MONITOR_HLS_PUBLIC_URL=https://mon1-hls.small.mxl.ipla.media.int
```

Leave both unset when opening the UI directly at `http://<node>:<WEB_PORT>`. The page then keeps `http://<MONITOR_PUBLIC_IP>:<port>/chN/...` and only replaces the hostname with the page's hostname.

## Run a demo

```bash
docker compose -f docker/docker-compose.demo.yaml up --build
```

The demo starts a registration stand-in, a v210/float32 test writer, MediaMTX, the monitor, Prometheus and Grafana. Open `http://127.0.0.1:8100/`.

Route the test flow to channel 1 (the ids below are the ones the writer creates):

```bash
RECEIVERS=$(curl -sf http://127.0.0.1:3242/x-nmos/node/v1.3/receivers)
VIDEO=$(python3 -c 'import json,sys; print(next(i["id"] for i in json.load(sys.stdin) if "Video" in i["label"]))' <<<"$RECEIVERS")
curl -sf -X PATCH "http://127.0.0.1:3242/x-nmos/connection/v1.2/single/receivers/${VIDEO}/staged" \
  -H 'Content-Type: application/json' \
  -d '{
    "master_enable": true,
    "activation": {"mode": "activate_immediate"},
    "transport_params": [{
      "mxl_domain_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
      "mxl_flow_id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    }]
  }'
```

Repeat for the audio receiver if you want sound. The multiviewer plays WHEP and falls back to HLS. Audio is muted in the browser until you unmute a tile. There is no source picker: routing is IS-05 only.

`docker/docker-compose.host.yaml` is the same pair of processes against `/Volumes/mxl` and a facility registry.

## Kubernetes

`deploy/mxl-webrtc-monitor.yaml` is a one-replica Deployment with the monitor and MediaMTX in one pod, host networking, the MXL root mounted read-only, a writable `/config`, probes on `/livez` and `/readyz`, and uid 1000. Pin it to a node with `nodeName` in the platform overlay. GPU is optional: set `runtimeClassName: nvidia` and `nvidia.com/gpu: 1`. The process starts on CPU encoding when NVENC is absent.

`deploy/monitoring/servicemonitor.yaml` scrapes `/metrics`. `deploy/grafana/mxl-webrtc-monitor.json` graphs state, lag, drops, encode fps, latency, bitrate and viewers. MediaMTX metrics are a separate scrape.

## Build

See `AGENTS.md`.

## Hardware check (not in CI)

On an A4000 and an L4: `ENCODER=auto` with 4 channels and with 16 channels, WebRTC from an operator desk, and HLS with UDP blocked. Confirm `encoder` in `/api/v1/channels` is `nvenc`, and that a channel whose NVENC session fails reports `x264` and increments `mxl_webrtc_monitor_encoder_fallbacks_total`.

Lab run 2026-10-03 on an NVIDIA A16 (not a target GPU; one GA107 of the board, driver 595.84, MediaMTX 1.21.1, 2× Xeon Gold 6136). Sources were 1080p50 v210 flows (mxl-test-player outputs with audio on channels 1–4, solid-colour writers on the rest), default preview 540p at source rate, 2 Mbit/s. Each case ran 15 s warm-up and 40–60 s measured.

| Encoder | Channels | `encoder` reported | Encoded fps (sum) | Mean encode latency | Drops | Fallbacks | Process CPU | NVENC load |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| auto | 4 | nvenc ×4 | 200 | 11.3 ms | 0 | 0 | 2.6 cores | 8 % |
| auto | 16 | nvenc ×16 | 802 | 12.6 ms | 0 | 0 | 10.2 cores | 31 % |
| x264 | 16 | x264 ×16 | 798 | 12.7 ms | 0 | 0 | 14.6 cores | – |

Image 1.0.0 did not pass: `nvh264enc` failed with "Selected preset not supported" on every channel and retried forever, and the MediaMTX sidecar did not start from the Compose file. Both are fixed (see CHANGELOG). The fallback path was checked by building the legacy preset back in: each channel logged `encoder_fallback`, reported `x264` and `encoder_fallbacks_total` 1. The LL-HLS playlist was fetched with curl. Browser WebRTC from an operator desk and HLS with UDP blocked were not run.

Lab run 2026-10-04, same host and method, 40 s measured: the preview made from v210 before GStreamer (1.0.1) against the image above.

| Encoder | Channels | Image | Encoded fps (sum) | Mean encode latency | Drops | Process CPU | NVENC load |
| --- | --- | --- | --- | --- | --- | --- | --- |
| auto | 16 | before | 797 | 13.0 ms | 0 | 10.4 cores | 31 % |
| auto | 16 | 1.0.1 | 799 | 3.0 ms | 0 | 6.4 cores | 30 % |
| x264 | 8 | before | 401 | 12.5 ms | 0 | 6.2 cores | – |
| x264 | 8 | 1.0.1 | 400 | 2.8 ms | 0 | 4.2 cores | – |

Lab run 2026-10-05, same host and method, 40 s measured: 1.0.3 (text overlay set only on change, faster preview conversion) against 1.0.2.

| Encoder | Channels | Image | Encoded fps (sum) | Mean encode latency | Drops | Process CPU | NVENC load |
| --- | --- | --- | --- | --- | --- | --- | --- |
| auto | 16 | 1.0.2 | 801 | 3.1 ms | 0 | 6.4 cores | 31 % |
| auto | 16 | 1.0.3 | 798 | 1.8 ms | 0 | 4.1 cores | 31 % |
| x264 | 8 | 1.0.2 | 399 | 2.9 ms | 0 | 4.2 cores | – |
| x264 | 8 | 1.0.3 | 401 | 1.4 ms | 0 | 3.2 cores | – |
| x264 | 16 | 1.0.2 | 799 | 2.9 ms | 0 | 8.9 cores | – |
| x264 | 16 | 1.0.3 | 799 | 1.4 ms | 0 | 6.3 cores | – |

With x264, most of the remaining CPU is the encoder itself (ultrafast preset); `v210ToPreview` is about a third.
