# syntax=docker/dockerfile:1
# Pins match mxl-fabrics-agent / mxl-decklink. See IMPLEMENTATION_PLAN.md.

FROM node:22-bookworm AS webui
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-fund --no-audit
COPY web/ ./
RUN npm run build

FROM ubuntu:24.04 AS build
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
ARG NMOS_CPP_REF=fe303849527394b03bdedc8f161f377fe458bb62

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config curl zip unzip tar ca-certificates \
        bison flex autoconf automake libtool \
        libboost-system-dev libboost-thread-dev libboost-chrono-dev \
        libboost-date-time-dev libboost-regex-dev libboost-random-dev \
        libboost-filesystem-dev libcpprest-dev nlohmann-json3-dev \
        libssl-dev libavahi-compat-libdnssd-dev \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-good1.0-dev \
        libgstrtspserver-1.0-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

RUN git init /tmp/mxl \
    && git -C /tmp/mxl remote add origin https://github.com/dmf-mxl/mxl.git \
    && git -C /tmp/mxl fetch --depth 1 origin "${MXL_REF}" \
    && git -C /tmp/mxl checkout FETCH_HEAD
COPY docker/mxl-vcpkg.json /tmp/mxl-manifest/vcpkg.json
RUN BASELINE="$(git -C /opt/vcpkg rev-parse HEAD)" \
    && sed -i "1a\\  \"builtin-baseline\": \"${BASELINE}\"," /tmp/mxl-manifest/vcpkg.json
RUN cmake -S /tmp/mxl -B /tmp/mxl/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_DIR=/tmp/mxl-manifest \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
        -DMXL_ENABLE_FABRICS_OFI=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /tmp/mxl/build -j"$(nproc)" \
    && cmake --install /tmp/mxl/build

RUN git init /opt/nmos-cpp \
    && git -C /opt/nmos-cpp remote add origin https://github.com/sony/nmos-cpp.git \
    && git -C /opt/nmos-cpp fetch --depth 1 origin "${NMOS_CPP_REF}" \
    && git -C /opt/nmos-cpp checkout FETCH_HEAD

COPY . /src
COPY --from=webui /web/dist /src/web/dist
RUN cmake -S /src -B /src/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="/opt/mxl;/tmp/mxl/build/vcpkg_installed/x64-linux" \
        -DMWM_WITH_NMOS=ON -DNMOS_CPP_DIR=/opt/nmos-cpp/Development \
        -DMWM_USE_PREBUILT_WEBUI=ON \
    && cmake --build /src/build -j"$(nproc)" \
    && /src/build/unit-tests

FROM ubuntu:24.04 AS runtime
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl \
        libcpprest2.10 libboost-chrono1.83.0t64 libboost-thread1.83.0 \
        libssl3t64 libavahi-compat-libdnssd1 \
        gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-rtsp gstreamer1.0-x \
        gstreamer1.0-tools libgstrtspserver-1.0-0 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/mxl/lib/ /usr/local/lib/
COPY --from=build /src/build/mxl-webrtc-monitor /usr/local/bin/mxl-webrtc-monitor
COPY --from=build /src/build/mxl-flow-writer /usr/local/bin/mxl-flow-writer
COPY tests/integration/fake_registry.py /usr/local/share/mxl-webrtc-monitor/fake_registry.py
RUN ldconfig \
    && if ! getent passwd 1000 >/dev/null; then \
         useradd --uid 1000 --no-create-home --shell /usr/sbin/nologin mxl; \
       fi
USER 1000:1000
EXPOSE 8100 3242 3243
ENTRYPOINT ["/usr/local/bin/mxl-webrtc-monitor"]
