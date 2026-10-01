# syntax=docker/dockerfile:1
#
# zigbee2supla container image.
#
# Targets:
#   runtime (default) - standalone container configured with Z2S_* variables
#                       (e.g. next to supla-docker, docker/supla-docker/)
#   addon             - Home Assistant add-on image; build with
#                       --build-arg BUILD_FROM=ghcr.io/home-assistant/<arch>-base
#
# The build stage uses the same base as the final image, so the binary always
# matches its C library (both default images are Alpine/musl).

ARG BUILD_FROM=alpine:3.21

FROM ${BUILD_FROM} AS build
RUN apk add --no-cache build-base cmake ninja openssl-dev mosquitto-dev \
      nlohmann-json yaml-cpp-dev gtest-dev
WORKDIR /src
COPY . .
ARG Z2S_RUN_TESTS=1
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DZ2S_BUILD_TESTS=$([ "${Z2S_RUN_TESTS}" = 1 ] && echo ON || echo OFF) \
    && cmake --build /build \
    && if [ "${Z2S_RUN_TESTS}" = 1 ]; then \
         ctest --test-dir /build --output-on-failure; \
       fi

# --- Home Assistant add-on -----------------------------------------------
FROM ${BUILD_FROM} AS addon
RUN apk add --no-cache libstdc++ openssl mosquitto-libs yaml-cpp tzdata jq
COPY --from=build /build/zigbee2supla /usr/bin/zigbee2supla
COPY zigbee2supla/run.sh /run.sh
RUN chmod a+x /run.sh
CMD ["/run.sh"]

# --- Standalone container (default) ---------------------------------------
FROM ${BUILD_FROM} AS runtime
RUN apk add --no-cache libstdc++ openssl mosquitto-libs yaml-cpp su-exec \
      tzdata \
    && adduser -D -H -s /sbin/nologin z2s \
    && mkdir -p /data && chown z2s:z2s /data
COPY --from=build /build/zigbee2supla /usr/bin/zigbee2supla
COPY docker/entrypoint.sh /entrypoint.sh
ENV Z2S_STATE_DIR=/data
# Time zone of the timestamps in the log; override with TZ in the env file.
ENV TZ=Europe/Warsaw
VOLUME /data
LABEL org.opencontainers.image.title="zigbee2supla" \
      org.opencontainers.image.description="Every ZigBee device as a separate Supla device" \
      org.opencontainers.image.source="https://github.com/goral64/ZigBee2Supla" \
      org.opencontainers.image.licenses="GPL-2.0-or-later"
ENTRYPOINT ["/entrypoint.sh"]
