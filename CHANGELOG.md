# Changelog

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
