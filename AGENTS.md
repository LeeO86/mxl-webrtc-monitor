# mxl-webrtc-monitor

NMOS-controlled preview of MXL flows in a browser. `SPECIFICATION.md` is the contract.
`IMPLEMENTATION_PLAN.md` records pins and deviations.

## Build

The image build in `docker/Dockerfile` is the supported path. A local build needs:

- CMake ≥ 3.24, Ninja, GCC ≥ 12 or Clang ≥ 16
- GStreamer 1.24 (base, good, bad, ugly, libav, rtsp, x) and the development packages
- MXL `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` built with `-DMXL_ENABLE_FABRICS_OFI=OFF`
- Sony nmos-cpp at `fe303849527394b03bdedc8f161f377fe458bb62` when `-DMWM_WITH_NMOS=ON`
- Node.js 22 to embed the Vue UI

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/opt/mxl \
  -DMWM_WITH_NMOS=ON \
  -DNMOS_CPP_DIR=/opt/nmos-cpp/Development
cmake --build build
./build/unit-tests
LD_LIBRARY_PATH=/opt/mxl/lib tests/integration/monitor.sh
```

`MXL_DOMAIN_SCAN_PATH` must be a directory of MXL domains. The process stays up if the mount is missing; `/readyz` stays false until it appears.

## Runtime notes

- Default web port is 8100. NMOS is 3242 and the WebSocket is 3243.
- MediaMTX is a sidecar. The process writes its config to `MEDIAMTX_CONFIG_PATH` on startup.
- WebRTC needs UDP to `MEDIAMTX_ICE_UDP_PORT` (8189). HLS on 8888 works over TCP when UDP is blocked.
- Two monitors on one host need distinct web, NMOS, RTSP, HLS, WHEP and ICE ports.
