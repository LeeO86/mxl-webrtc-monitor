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

GStreamer packages on Ubuntu 24.04: `gstreamer1.0-plugins-base`, `gstreamer1.0-plugins-good`, `gstreamer1.0-plugins-bad`, `gstreamer1.0-plugins-ugly`, `gstreamer1.0-libav`, `gstreamer1.0-rtsp`, `gstreamer1.0-x`, `gstreamer1.0-tools`, plus the matching `-dev` packages and `libgstrtspserver-1.0-dev` at build time. `textoverlay` comes from `gstreamer1.0-x`. `nvh264enc` is selected when that element factory reaches `READY`; the Ubuntu package set does not ship a working NVENC plugin without the NVIDIA userspace driver, and the process falls back to x264.

## 2. Deviations

1. **v210 unpack is GStreamer's CPU `v210` raw format** (`video/x-raw,format=v210` into `videoconvert`). There is no separate GPU unpack. `video/v210a` is accepted: the fill plane is pushed as v210 and the alpha plane is ignored.
2. **Default NMOS seed is `HOST_ID-monitor`**, matching the configuration table. The prose also mentions the container name; under host networking `HOST_ID` is the host or pod name. Set `NMOS_SEED` to override it.
3. **IDs are UUIDv5** in the RFC 4122 URL namespace. The name is `mxl-webrtc-monitor/<seed>/node` (and `/device`, `/ch/<n>/video`, `/ch/<n>/audio`).
4. **Audio meters are computed in the reader** (peak and RMS, about 10 Hz), not by the GStreamer `level` element. `METRICS_AUDIO_PEAK` defaults to false and is not in the spec table.
5. **Opus is the only audio codec.** MediaMTX 1.20.1 documents Opus as an HLS codec, so no AAC track is added.
6. **The Query API is assumed at `NMOS_REGISTRY_PORT + 1`.** That matches the nmos-cpp registry defaults (registration 3210, query 3211). Sender labels for the overlay are read from that API.
7. **A missing MXL root does not exit 78.** Exit 78 is invalid configuration. Readiness stays false until the directory exists, NMOS is registered or disabled, and the MediaMTX API answers.
8. **Unrouted audio omits the audio track.** Routed audio that has not produced samples yet pushes digital silence so the player keeps a continuous timeline.
9. **DNS-SD off** sets nmos-cpp `pri` and `highest_pri` to `no_priority`, which disables advertisement and discovery. `NMOS_DNS_SD=true` leaves the nmos-cpp defaults.
10. **Per-channel settings** are `CH<n>_*` environment keys and a `channels` array in the JSON file. They apply at runtime. Global keys changed through the UI set `restart_required`.
11. **x264 uses `ultrafast` and `tune=zerolatency`**, GOP of about one second, no B-frames. NVENC asks for CBR, `low-latency-hq`, and `zerolatency` when the element accepts those properties.
12. **The demo Compose registry is the Python stand-in** in `tests/integration/fake_registry.py`. It implements the registration and query calls this process makes. A facility deployment sets `NMOS_REGISTRY_ADDRESS` to a real registry.

## 3. Process

One nmos-cpp node exposes a video receiver and an audio receiver per channel. Each channel has a video reader thread and an audio reader thread. They resolve the domain on every attempt (no negative cache), open the MXL reader, and push into a leaky `appsrc`. A slow encoder drops the oldest buffer and increments `grains_dropped_total{reason="queue_full"}`. Falling out of the ring resynchronises to head minus `READ_OFFSET_GRAINS` and increments `resyncs_total`.

The video thread publishes an H.264 elementary stream. The audio thread publishes Opus when that receiver is enabled. Both go to `rtsp://127.0.0.1:<rtsp>/ch<n>` over TCP. MediaMTX serves WHEP and low-latency HLS.

## 4. Tests

- Unit tests cover config precedence, IS-05 UUID validation, UUIDv5 ids, the channel state machine, domain scan including mirror domains, audio pair selection, backoff, and public WHEP/HLS URL parsing plus the channel playback JSON.
- `tests/integration/monitor.sh` writes a v210 and float32 flow, PATCHes channel 1, waits for `running`, checks that the HLS playlist grows segments, stops the writer and expects `no_signal`, activates a missing flow and expects `waiting`, then creates that flow and expects `running` without another PATCH. It sets `MONITOR_HLS_PUBLIC_URL` and fetches the playlist through the URL the API reports.
- Hardware checks in spec §10 (NVENC on A4000 and L4, 4 and 16 channels, browser WebRTC, HLS with UDP blocked) are not run in CI.

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
