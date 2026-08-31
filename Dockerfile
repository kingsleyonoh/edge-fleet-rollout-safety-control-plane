# Later production packaging. The standalone development gate remains Docker-free.
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    VCPKG_ROOT=/opt/vcpkg \
    PATH=/opt/cmake/bin:/opt/vcpkg:$PATH

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates curl git ninja-build pkg-config python3 python3-pip \
      tar unzip zip && \
    python3 -m pip install --break-system-packages --no-cache-dir cmake==3.30.5 && \
    git init "$VCPKG_ROOT" && \
    git -C "$VCPKG_ROOT" remote add origin https://github.com/microsoft/vcpkg.git && \
    git -C "$VCPKG_ROOT" fetch --depth 1 origin 114d9fe62faf35856b45cf55cb93b57028a45d63 && \
    git -C "$VCPKG_ROOT" checkout --detach FETCH_HEAD && \
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY vcpkg.json CMakeLists.txt CMakePresets.json ./
COPY triplets ./triplets
RUN vcpkg install --triplet x64-linux-release --overlay-triplets=/src/triplets

COPY src ./src
COPY web ./web
COPY config ./config
COPY migrations ./migrations
COPY tests ./tests
COPY fixtures ./fixtures
RUN apt-get update && apt-get install -y --no-install-recommends bison flex && \
    rm -rf /var/lib/apt/lists/*
RUN cmake --preset production \
      -DVCPKG_TARGET_TRIPLET=x64-linux-release \
      -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
      -DVCPKG_OVERLAY_TRIPLETS=/src/triplets && \
    cmake --build --preset production -j 2 && \
    ctest --preset production --output-on-failure

FROM ubuntu:24.04 AS production

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl libssl3 && \
    rm -rf /var/lib/apt/lists/* && \
    groupadd --gid 10001 edgefleet && \
    useradd --uid 10001 --gid edgefleet --no-create-home --home-dir /app --shell /usr/sbin/nologin edgefleet

WORKDIR /app
COPY --from=builder --chown=10001:10001 /src/vcpkg_installed/x64-linux-release/lib /usr/local/lib
RUN ldconfig
COPY --from=builder --chown=10001:10001 /src/build/production/edgefleet /app/edgefleet
COPY --from=builder --chown=10001:10001 /src/migrations /app/migrations
COPY --from=builder --chown=10001:10001 /src/web /app/web
COPY --from=builder --chown=10001:10001 /src/config /app/config

USER 10001:10001
EXPOSE 8080 9090
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
  CMD curl --fail --silent http://127.0.0.1:8080/health/ready >/dev/null || exit 1

ENTRYPOINT ["/app/edgefleet"]
CMD ["serve"]
