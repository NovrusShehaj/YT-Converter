# Optional personal-use image. Default bind is 127.0.0.1.
# Publishing 80/443 to the Internet is out of scope without legal review
# and requires YTCONV_ALLOW_REMOTE=1 plus YTCONV_API_KEY.

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        libboost-dev \
        libcpprest-dev \
        libssl-dev \
        pkg-config \
        ffmpeg \
        python3 \
        python3-pip \
        curl \
    && pip3 install --break-system-packages --no-cache-dir yt-dlp \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt /src/CMakeLists.txt
COPY include /src/include
COPY src /src/src

RUN cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
    && cmake --build /tmp/build --parallel \
    && install -m 0755 /tmp/build/yt2mp3-cli /usr/local/bin/yt2mp3-cli \
    && if [ -f /tmp/build/yt2mp3-api ]; then install -m 0755 /tmp/build/yt2mp3-api /usr/local/bin/yt2mp3-api; fi \
    && rm -rf /src /tmp/build

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
