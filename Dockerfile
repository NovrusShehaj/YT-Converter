# Build stage
FROM debian:bookworm-slim AS builder

# make is listed explicitly: cmake only recommends it, and recommends are not installed.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        make \
        libboost-dev \
        libcpprest-dev \
        libssl-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt /src/CMakeLists.txt
COPY include /src/include
COPY src /src/src
COPY docker /src/docker

# BUILD_API=ON makes a missing cpprestsdk a configure error, so the API binary always exists.
# The runtime package list is derived from the built binaries' complete ldd closure.
RUN cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_API=ON \
    && cmake --build /tmp/build --parallel \
    && install -m 0755 /tmp/build/yt2mp3-cli /tmp/build/yt2mp3-api /usr/local/bin/ \
    && sh docker/collect-runtime-packages.sh /usr/local/bin/yt2mp3-cli /usr/local/bin/yt2mp3-api \
        > /tmp/runtime-packages.txt \
    && cat /tmp/runtime-packages.txt

# Runtime stage: no compiler toolchain.
FROM debian:bookworm-slim

COPY --from=builder /tmp/runtime-packages.txt /tmp/runtime-packages.txt
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        ffmpeg \
        python3 \
        python3-pip \
        curl \
        $(cat /tmp/runtime-packages.txt) \
    && pip3 install --break-system-packages --no-cache-dir yt-dlp==2024.12.23 \
    && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages.txt

COPY --from=builder /usr/local/bin/yt2mp3-cli /usr/local/bin/yt2mp3-cli
COPY --from=builder /usr/local/bin/yt2mp3-api /usr/local/bin/yt2mp3-api
COPY docker/verify-runtime.sh /usr/local/lib/ytconv/verify-runtime.sh

# Fails the image build on any unresolved shared library or a present compiler.
RUN sh /usr/local/lib/ytconv/verify-runtime.sh /usr/local/bin/yt2mp3-cli /usr/local/bin/yt2mp3-api

LABEL org.opencontainers.image.title="YT-Converter" \
      org.opencontainers.image.description="Local YouTube converter" \
      org.opencontainers.image.version="1.0.0" \
      io.github.yt-converter.yt-dlp.version="2024.12.23"

RUN useradd --create-home --uid 10001 --shell /usr/sbin/nologin ytconv \
    && mkdir -p /data/output \
    && chown -R ytconv:ytconv /data

USER ytconv
WORKDIR /data
ENV YTCONV_BIND=127.0.0.1 \
    YTCONV_PORT=8080 \
    YTCONV_OUTPUT_DIR=/data/output \
    YTCONV_LOG_LEVEL=INFO \
    YTCONV_ALLOW_UNAUTHENTICATED_LOCALHOST=1

EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/v1/healthz >/dev/null || exit 1

ENTRYPOINT ["yt2mp3-api"]
