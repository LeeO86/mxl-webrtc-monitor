# Changelog

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
