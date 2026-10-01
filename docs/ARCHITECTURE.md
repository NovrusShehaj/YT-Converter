# Architecture

YT-Converter is a small local tool: two binaries share a static core library. It downloads one YouTube video with `yt-dlp` and transcodes with `ffmpeg`. There is no frontend and no database. The API keeps an in-process queue so HTTP threads are not blocked on a download.

## Binaries

```
yt2mp3-cli          src/cli/main.cpp
yt2mp3-api          src/api/server.cpp + src/api/api_app.cpp
        \                 /
         \               /
      yt-converter-core (STATIC)
        converter.cpp
        validation.cpp
        process.cpp      posix_spawnp / CreateProcessW
        logger.cpp
        config.cpp
        error.cpp
        dependencies.cpp
        job_limiter.cpp
        job_queue.cpp
        singleflight.cpp
        metrics.cpp
```

## Request flow

1. CLI argv or `POST /v1/conversions` JSON `{url, format}`.
2. `yt::validation::parseYouTubeUrl` full-string parse. Allowed hosts: `youtube.com`, `www.`, `m.`, `music.`, `youtu.be`. Allowed paths: `/watch?v=`, `/shorts/`, `/embed/`, `/live/`, `youtu.be/<id>`. ID must be `^[A-Za-z0-9_-]{11}$`. Playlist-only and channel URLs fail with typed errors.
3. Download URL is always `https://www.youtube.com/watch?v=<id>`. Original user input is not passed to yt-dlp.
4. `yt::process::run(argv)` starts the child with a real argv array. No `system()`, `popen()`, or shell concatenation.
5. Audio downloads land in `<output_root>/cache/src/<id>/audio/` and are shared by MP3 and WAV. MP4 is published with a hard link or copy when yt-dlp already wrote an `.mp4`; other containers are remuxed with ffmpeg. Finished files are `<output_root>/<id>.<fmt>`. Each writer encodes into its own hidden temporary file, `<output_root>/.<id>.<fmt>.<random>.partial`, with an explicit ffmpeg muxer (`-f mp3`, `-f wav`, or `-f mp4`) because the `.partial` name does not identify a container. Success renames it over the final name; failure or cancellation removes it and leaves any previous final file untouched. A crash can leave a `.partial`, never a truncated final name.
6. CLI prints the absolute path. `POST /v1/conversions` returns `202` and a job URL. `GET /v1/jobs/{id}` reports stage, percent, and timings.

## Process execution

`src/utils/process.cpp` uses `posix_spawnp` (Linux/macOS) with a new process group, stdin from `/dev/null`, bounded stdout/stderr capture, and a deadline. Timeout or API/CLI shutdown sends SIGTERM/SIGKILL to the group. Windows uses `CreateProcessW` without `cmd.exe`.

yt-dlp flags include `--no-playlist`, `--newline`, `--socket-timeout`, `--max-filesize`, `--retries`, `--concurrent-fragments`, `--fragment-retries`, `--retry-sleep`, `--cache-dir`, `--no-mtime`, and `--merge-output-format mp4` for MP4. Audio formats use an audio-only selector (`ba[ext=m4a]/ba[ext=webm]/ba[ext=opus]/ba[acodec!=none]`) with no combined-format fallback. MP4 uses a height-capped mp4+m4a selector.

ffmpeg flags include `-y -nostdin -hide_banner -loglevel error`.

## Concurrency and shutdown

The API runs `YTCONV_MAX_CONCURRENT` workers (default 2) behind a queue of depth `YTCONV_QUEUE_DEPTH` (default 8). A full queue returns 503. Identical in-flight video and format pairs share one download. Cancel sends SIGTERM, waits up to 2 seconds, then SIGKILL for that child only.

`SIGINT` and `SIGTERM` stop the listener and cancel tracked children. New conversions after shutdown return 503. High-frequency probes use `/v1/healthz`. `/v1/readyz` caches tool checks for `YTCONV_READY_TTL_SEC`.

## Configuration and security defaults

Config is environment (`YTCONV_*`) plus a few CLI flags. Default bind is loopback. Wildcard bind requires `YTCONV_ALLOW_REMOTE=1` and `YTCONV_API_KEY`. Conversion is POST-only. Logger is mutex-protected; INFO logs IDs, not URLs, unless `YTCONV_LOG_URLS=1` at DEBUG.

## Residual risk

yt-dlp still talks to YouTube (and whatever extractors it follows). Huge or live media can still take time and disk up to the configured caps. A browser on the same machine can CSRF an unauthenticated localhost API; use an API key if that matters. Hosting this for other people is a legal and abuse problem, not just a code problem.
