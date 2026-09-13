# Architecture

YT-Converter is a small local tool: two binaries share a static core library. It downloads one YouTube video with `yt-dlp` and transcodes with `ffmpeg`. There is no frontend, database, queue, or cluster.

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
        metrics.cpp
```

## Request flow

1. CLI argv or `POST /v1/conversions` JSON `{url, format}`.
2. `yt::validation::parseYouTubeUrl` full-string parse. Allowed hosts: `youtube.com`, `www.`, `m.`, `music.`, `youtu.be`. Allowed paths: `/watch?v=`, `/shorts/`, `/embed/`, `/live/`, `youtu.be/<id>`. ID must be `^[A-Za-z0-9_-]{11}$`. Playlist-only and channel URLs fail with typed errors.
3. Download URL is always `https://www.youtube.com/watch?v=<id>`. Original user input is not passed to yt-dlp.
4. `yt::process::run(argv)` starts the child with a real argv array. No `system()`, `popen()`, or shell concatenation.
5. Work happens under `<output_root>/jobs/<job_id>/`. On success the final file is `<output_root>/<id>.<fmt>` and temps are deleted. On failure temps and incomplete output are deleted.
6. CLI prints the absolute path. API returns JSON with `output_path` and `job_id`.

## Process execution

`src/utils/process.cpp` uses `posix_spawnp` (Linux/macOS) with a new process group, stdin from `/dev/null`, bounded stdout/stderr capture, and a deadline. Timeout or API/CLI shutdown sends SIGTERM/SIGKILL to the group. Windows uses `CreateProcessW` without `cmd.exe`.

yt-dlp flags include `--no-playlist`, `--newline`, `--socket-timeout`, `--max-filesize`, `--retries`. Audio formats use `bestaudio/best`. MP4 uses a height-capped mp4+m4a selector.

ffmpeg flags include `-y -nostdin -hide_banner -loglevel error`.

## Concurrency and shutdown

The API uses a process-wide `JobLimiter` (`YTCONV_MAX_CONCURRENT`, default 1). Same video ID is serialized with a per-ID mutex. Distinct jobs never share temp paths.

`SIGINT` and `SIGTERM` stop the listener, set a shutdown flag, and kill tracked children. New conversions after shutdown return 503.

## Configuration and security defaults

Config is environment (`YTCONV_*`) plus a few CLI flags. Default bind is loopback. Wildcard bind requires `YTCONV_ALLOW_REMOTE=1` and `YTCONV_API_KEY`. Conversion is POST-only. Logger is mutex-protected; INFO logs IDs, not URLs, unless `YTCONV_LOG_URLS=1` at DEBUG.

## Residual risk

yt-dlp still talks to YouTube (and whatever extractors it follows). Huge or live media can still take time and disk up to the configured caps. A browser on the same machine can CSRF an unauthenticated localhost API; use an API key if that matters. Hosting this for other people is a legal and abuse problem, not just a code problem.
