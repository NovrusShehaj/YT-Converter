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

## Source cache, leases, and output writers

Downloaded sources are immutable *generations* under `<output>/cache/src/<id>/<kind>/gen-<stamp>-<rand>/source.<ext>` (`kind` is `audio`, shared by MP3 and WAV, or `v<height>` for MP4). A download writes into its own `inc-<stamp>-<rand>/` directory, is verified, and is then renamed into a new generation; a refresh never deletes or overwrites the generation another reader is using, and a failed refresh leaves the previous generation and output usable.

A conversion holds a **lease** (a shared advisory lock on the generation's `.lease` file) from lookup until its encode or MP4 publication finishes, including on exceptions and cancellation (RAII). Lookup-and-lease, publication, and eviction all run under a short-lived cache metadata lock (`cache/src/.lock`), and eviction deletes a generation only after taking an exclusive `.lease` lock without waiting. Because the locks are `flock()` locks (LockFileEx on Windows) on close-on-exec descriptors, the rule holds between threads and between separate CLI/API processes sharing an output root, and the kernel releases a dead process's locks: its `inc-*` directory becomes removable and no manual recovery is needed. Spawned children never inherit a lock.

Eviction (`SourceCache::maintain`, run after each download and after each lease release) removes, in order: abandoned `inc-*` directories, retired generations (any but the newest per video/kind), expired generations (`YTCONV_SOURCE_CACHE_TTL_SEC`), and then unleased current generations oldest-first while retained plus leased bytes exceed `YTCONV_SOURCE_CACHE_MAX_BYTES`. Leased entries are never deleted; they are deferred and reported as pressure (`leased_bytes` vs `retained_bytes`, plus a warning log). A source larger than the cap can be a transient leased input but is removed as soon as it is released, so idle maintenance restores the retained-byte bound. Active working bytes are bounded separately by `YTCONV_MAX_CONCURRENT` × `YTCONV_MAX_FILESIZE`. Hidden `*.partial` outputs older than the convert timeout plus one hour (left by a crashed process) are removed too.

Lock order: the cache metadata lock is only held for directory operations and never while running a child, waiting on a shared download, or holding a queue lock. A per-output writer lock (`cache/locks/<id>.<fmt>.lock`, cancellation-aware) is the only lock held across a child (the encode), so competing writers to one output are serialized across threads and processes.

## Process execution

`src/utils/process.cpp` uses `posix_spawnp` (Linux/macOS) with a new process group, stdin from `/dev/null`, bounded stdout/stderr capture, and a deadline. Timeout or API/CLI shutdown sends SIGTERM/SIGKILL to the group. Windows uses `CreateProcessW` without `cmd.exe`.

Every retained output buffer is bounded. Captured text keeps the last `max_output_bytes` (8 KiB) per stream. Progress callbacks get lines of at most `max_line_bytes` (8 KiB, separate from capture): an oversized line keeps its first 8 KiB, the remainder is discarded up to the newline while the pipe keeps draining, and one truncated callback is delivered; a final unterminated line follows the same rule. Read ends are nonblocking, so a grandchild holding a pipe open cannot hang the final drain. If a callback throws, an RAII guard kills and reaps the child's process group before the exception propagates. The POSIX path is covered by `ProcessLines.*` tests using the compiled `ytconv-output-writer` fixture; the Windows path implements the same policy but is not built or tested in CI.

yt-dlp flags include `--no-playlist`, `--newline`, `--socket-timeout`, `--max-filesize`, `--retries`, `--concurrent-fragments`, `--fragment-retries`, `--retry-sleep`, `--cache-dir`, `--no-mtime`, and `--merge-output-format mp4` for MP4. Audio formats use an audio-only selector (`ba[ext=m4a]/ba[ext=webm]/ba[ext=opus]/ba[acodec!=none]`) with no combined-format fallback. MP4 uses a height-capped mp4+m4a selector.

ffmpeg flags include `-y -nostdin -hide_banner -loglevel error`.

## Concurrency and shutdown

The API separates **client jobs** from **operations**. A job is one client's independently cancelable request and its compact, bounded status record. An operation owns one execution (download and encode), its own cancellation token, and its subscriber list; it is never owned by the job that created it, so it survives that job's cancellation or pruning. `YTCONV_MAX_CONCURRENT` workers (default 2) execute operations; `YTCONV_QUEUE_DEPTH` (default 8) bounds queued plus running operations; `YTCONV_MAX_ACTIVE_JOBS` bounds live job records (checked before an attached job is allocated); terminal snapshots are bounded by `YTCONV_JOB_HISTORY_MAX` and expire `YTCONV_JOB_HISTORY_TTL_SEC` after completion on a monotonic clock. Full limits return 503.

Requests of the same video, format, and policy class (`reuse`, `replace`, or `refresh`) attach to one operation. Different formats of one video share a download through a cancellation-aware single-flight: waiters poll their own token, and the download child is canceled only when every subscribed operation has canceled. Canceling a job detaches only that job; the last subscriber leaving cancels the operation (SIGTERM, 2 s grace, SIGKILL), a queued operation is dropped at once, and a running one keeps its worker slot until its child has actually exited. A canceled operation's key is released immediately, so a new request starts a new operation instead of joining work being torn down. Terminal job states never change. Shutdown rejects new submissions, cancels every job and operation, resolves terminal callbacks, and joins workers.

`SIGINT` and `SIGTERM` stop the listener and cancel tracked children. New conversions after shutdown return 503. High-frequency probes use `/v1/healthz`. `/v1/readyz` caches tool checks for `YTCONV_READY_TTL_SEC`.

## HTTP intake

`src/api/request_gate.cpp` owns the public socket. One poll-based thread reads each request with fixed limits (16 KiB of headers, 8 KiB of decoded body, `Content-Length` or chunked), a whole-request deadline, and caps on pending reads and open connections, then forwards the normalized request (`Content-Length`, `Connection: close`, a per-process secret, and the peer address) to cpprest's listener on a random loopback port. Responses are relayed back with a bounded buffer. This exists because cpprestsdk's asio listener buffers whole bodies itself and has no read timeout. The handler still reads the body asynchronously with its own 8,193-byte bound and never blocks a listener thread; shutdown resolves outstanding responses, closes the internal listener, waits for body-read continuations, and then closes the gate (dropping half-read requests).

The gate is POSIX-only. The API is not supported on Windows; the Windows process path in `process.cpp` is unverified.

## Configuration and security defaults

Config is environment (`YTCONV_*`) plus a few CLI flags. Default bind is loopback. Wildcard bind requires `YTCONV_ALLOW_REMOTE=1` and `YTCONV_API_KEY`. Conversion is POST-only. Logger is mutex-protected; INFO logs IDs, not URLs, unless `YTCONV_LOG_URLS=1` at DEBUG.

## Residual risk

yt-dlp still talks to YouTube (and whatever extractors it follows). Huge or live media can still take time and disk up to the configured caps. A browser on the same machine can CSRF an unauthenticated localhost API; use an API key if that matters. Hosting this for other people is a legal and abuse problem, not just a code problem.
