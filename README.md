# mxl-webrtc-monitor

Watch and listen to MXL flows in a browser. Each monitor channel is a pair of NMOS BCP-007-03 MXL receivers (video and audio). A controller routes any sender to a channel with IS-05. The process reads the local MXL domain, encodes a low-bitrate preview, and publishes it to a [MediaMTX](https://github.com/bluenviron/mediamtx) sidecar. The built-in page shows every channel, with labels, state and audio meters.

`mxl-fabrics-agent` sees the NMOS subscription and can replicate the flow onto this host. This process does not know about hosts or RDMA.

## Ports

| Port | Use |
| --- | --- |
| 8100 | UI, REST, `/livez`, `/readyz`, `/metrics` |
| 3242 / 3243 | NMOS Node API and its WebSocket |
| 8554 | RTSP ingest on localhost (MediaMTX) |
| 8889 | WHEP |
| 8888 | Low-latency HLS |
| 8189 | ICE, one UDP port and a TCP fallback on the same port |
| 9997 | MediaMTX API, localhost only |

These do not collide with mxl-decklink (8080, 3212), mxl-st2110-gateway (8090), mxl-fabrics-agent (8095, 3232) or FlowXer (9620). A second monitor on the same host needs its own web, NMOS, RTSP, HLS, WHEP and ICE ports, on both containers.

WebRTC needs UDP from the browser to the ICE port. If UDP is blocked, the page falls back to HLS, which is ordinary HTTP.

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

`deploy/mxl-webrtc-monitor.yaml` is a one-replica Deployment with the monitor and MediaMTX in one pod, host networking, the MXL root mounted read-only, and uid 1000. Pin it to a node with `nodeName` in the platform overlay. GPU is optional: set `runtimeClassName: nvidia` and `nvidia.com/gpu: 1`. The process starts on CPU encoding when NVENC is absent.

`deploy/monitoring/servicemonitor.yaml` scrapes `/metrics`. `deploy/grafana/mxl-webrtc-monitor.json` graphs state, lag, drops, encode fps, latency, bitrate and viewers. MediaMTX metrics are a separate scrape.

## Build

See `AGENTS.md`. Exit codes: 0 clean, 75 startup failed, 78 invalid configuration, 143 shutdown did not finish within 15 seconds.

## Hardware check (not in CI)

On an A4000 and an L4: `ENCODER=auto` with 4 channels and with 16 channels, WebRTC from an operator desk, and HLS with UDP blocked. Confirm `encoder` in `/api/v1/channels` is `nvenc`, and that a channel whose NVENC session fails reports `x264` and increments `mxl_webrtc_monitor_encoder_fallbacks_total`.
