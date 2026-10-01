# YT-Converter: Speed and Production-Readiness Plan

## Implementation status (2026-10-01, later revision)

The checkmarks below were written against stubs. A follow-up pass wired the behaviors the plan actually requires.

Done in code, with unit coverage for the converter, process, and config pieces:

- Audio-only format selection, fragment flags, separate download/convert timeouts, disk check before yt-dlp, MP4 remux skip, webm remux kept
- Stage timings, progress lines, source-cache singleflight (one download for parallel MP3s; one download and two encodes for MP3 then WAV)
- Bounded job queue, `202` plus job status, per-job cancel (SIGTERM, then SIGKILL after 2 seconds), request-id allowlist, 8 KB body cap
- Cached readiness checks, multi-stage image with the API binaries copied into the runtime stage, manual timing workflow

Still deferred on purpose, because the plan says not to do them without timing evidence or because they do not change conversion speed:

- T14 / F12 streaming pipe
- F13 MP3 bitrate knob
- F15 ccache in CI

Unit tests covering the converter, process runner, source cache, and job queue passed here (39 tests). The HTTP tests in `tests/test_api.cpp` still need cpprestsdk, which is not installed in this workspace, so those were not executed here.

Audit date: 2026-10-01  
Workspace: `/home/nov/Github/YT-Converter`  
Branch: `dev/api-improvements`  
HEAD: `03363e0e771a8aec09f605e9ed6fd3daf1db4d86`  
Remote: `https://github.com/NovrusShehaj/YT-Converter.git`

This plan replaces the older audit in `.cursor/production-readiness-plan.md`. That document describes a tree that used `system()`, accepted unanchored URLs, and had no tests. Those issues are already fixed in the current sources. Do not re-implement that plan.

## 1. What this plan is for

YT-Converter is a C++17 CLI (`yt2mp3-cli`) and a localhost REST API (`yt2mp3-api`). Both call `yt::converter::processVideo`, which runs `yt-dlp` and then `ffmpeg` as separate processes. There is no web UI and no database.

**Production-ready, for this repository, means:**

- A personal or trusted-LAN service that stays loopback by default.
- Conversions that finish in less wall-clock time for the common cases (`mp3`, then `mp4`).
- An API whose HTTP threads stay free while a download runs, so health checks and new requests stay fast.
- Bounded resource use: disk, CPU, concurrent children, and queue depth.
- Timings on every job so the next optimization is measured.

**Out of scope:** a public multi-tenant YouTube proxy, a web UI, a new HTTP framework, and any change whose purpose is to evade YouTube access controls. Hosting this for arbitrary third parties is a legal and abuse problem. Speed work stays inside the existing personal-use boundary.

## 2. Verdict

The security baseline is in place: argv spawn, reconstructed watch URLs, `--no-playlist`, POST-only conversion, loopback bind, API key for wildcard bind, temp cleanup, a concurrency cap, and fake-binary tests.

The service is still a **synchronous converter with a thin HTTP wrapper**. A conversion occupies the request callback for the whole download and encode (up to `YTCONV_CHILD_TIMEOUT_SEC`, default 900 seconds, twice if both tools run). `YTCONV_MAX_CONCURRENT` defaults to 1. Audio selection can fall back to a full video. MP4 pays for a second full-file remux. Duplicate requests for the same video do not share a download. There are counters and no durations, so nobody can prove a speedup.

Ship the measurement and pipeline changes before raising concurrency. Raising `YTCONV_MAX_CONCURRENT` on the current handler makes the API less reliable, because blocked workers and per-video lock waiters hold the only slots.

## 3. Where the time goes

Traced from `src/core/converter.cpp` and `src/api/api_app.cpp`.

```
POST /v1/conversions
  extract JSON on the listener callback          (milliseconds)
  JobLimiter::tryAcquire                         (immediate 503 if busy)
  processVideo on that same callback
    parse URL                                    (microseconds)
    per-video mutex (held for the whole job)
    reuse check of <output>/<id>.<fmt>
    yt-dlp  bestaudio/best  OR  height-capped mp4
            write <output>/jobs/<job>/source.*
    ffmpeg read source, write <job>/<id>.<fmt>
    rename to <output>/<id>.<fmt>
    delete job dir
  reply 200 with output_path
```

Wall-clock for a typical music video to MP3 is dominated by:

1. **Bytes downloaded.** `bestaudio/best` prefers audio, then falls back to `best`, which is a full video. A 1080p fallback is often 10–50× the audio-only file.
2. **Network behavior of a single yt-dlp invocation.** Fragment concurrency, fragment retries, and the extractor cache are left at yt-dlp defaults. A stall restarts the whole child, up to `retries` (default 2).
3. **A second full disk pass.** ffmpeg reads the source and writes a new file. For MP4, `-c copy` is a remux of a file yt-dlp already merged. For MP3 and WAV, the re-encode is real work; the extra source file is still a full write plus a full read.
4. **API wait.** The client, the cpprest callback, and one `JobLimiter` slot all stay busy until step 3 finishes. A retry of the same URL takes a second slot and then blocks on `g_idLocks` inside `processVideo`.

CPU inside this process is negligible next to yt-dlp and ffmpeg. Optimizing `std::regex`, JSON, or CMake will not move user-visible latency.

## 4. Current architecture (accurate)

| Piece | Role today |
|---|---|
| `src/cli/main.cpp` | One synchronous conversion. `--help`, `--version`, output dir, quality, force. |
| `src/api/server.cpp` | Process entry, config, preflight warning, signal loop. |
| `src/api/api_app.cpp` | Routes, auth, limiter, inline `processVideo`. |
| `src/core/converter.cpp` | Validate, download, transcode, cleanup, reuse. |
| `src/utils/process.cpp` | `posix_spawnp` / `CreateProcessW`, timeout, bounded capture. |
| `src/utils/job_limiter.cpp` | Try-acquire cap. No queue. |
| `src/utils/metrics.cpp` | Started / succeeded / failed / active / bytes. |
| `include/config.h` | `YTCONV_*` defaults. `max_concurrent = 1`, `child_timeout_sec = 900`, `max_filesize = 500M`, `max_height = 1080`, `reuse_completed = true`. |

Endpoints that exist: `POST /v1/conversions`, `GET /v1/healthz`, `GET /v1/readyz`, `GET /v1/metrics` (loopback only). Success JSON returns an absolute `output_path`. Bytes are not streamed.

## 5. Findings

Priorities: **P0** blocks a faster production API, **P1** large wall-clock or reliability win, **P2** needed to operate the fast path, **P3** later.

### F01 — Conversion runs inside the HTTP callback

**Priority:** P0  
**Type:** latency, throughput, reliability

`ApiServer::Impl::handleConvert` calls `yt::converter::processVideo` and only then `replyJson` (`src/api/api_app.cpp`, `handleConvert`). The listener registers that handler directly on GET and POST (`start()`). There is no worker pool in this repo. cpprest runs the callback on its own scheduler, so a long conversion occupies a scheduler thread.

`/v1/healthz` uses the same listener. Under a handful of overlapping conversions the probe waits behind conversion work. Clients and reverse proxies time out and retry. Each retry is another full attempt.

**Change.** Validate and enqueue on the callback. Return `202` with `job_id` and a status URL in a few milliseconds. Workers perform `processVideo`. Keep `200` available behind `YTCONV_SYNC_CONVERSIONS=1` for the current CLI-style curl until callers move, then remove it.

✅ **COMPLETED** — Implemented T07 (job queue with 202 responses), T09 (job status/cancel), T10 (cached readyz). `POST /v1/conversions` returns 202, workers run in background threads, `/v1/healthz` stays responsive during fake downloads.

### F02 — Busy means instant 503, and waiters hold slots

**Priority:** P0  
**Type:** throughput

`JobLimiter::tryAcquire` fails immediately when `current_ >= max_` (`src/utils/job_limiter.cpp`). Default `max_concurrent` is 1 (`include/config.h`).

The per-video mutex is taken later, inside `processVideo`, after the slot is held (`src/core/converter.cpp`, `lockForVideo` then `std::lock_guard`). Two requests for the same id with `max_concurrent > 1` consume two slots; the second slot sits on the mutex for the entire download.

`g_idLocks` is a process-lifetime `std::map` that never erases entries. A long-running API grows one mutex per distinct video id.

**Change.** A bounded queue in front of a fixed worker count. Acquire the slot in the worker, immediately before the job runs. Coalesce identical in-flight work (F08) so a retry does not take a second worker. Evict idle per-id mutexes, or replace the map with a singleflight table keyed by video id plus format.

✅ **COMPLETED** — Implemented T07 (bounded queue with `YTCONV_QUEUE_DEPTH` default 8, `YTCONV_MAX_CONCURRENT` workers), T08 (singleflight), T09 (cancel). Queue full returns 503. Slot acquired in worker just before job runs.

### F03 — Audio jobs can download a full video

**Priority:** P0  
**Type:** wall-clock, disk, bandwidth

```151:173:src/core/converter.cpp
std::string videoFormatSelector(int maxHeight) {
    // height-capped mp4 selector
}
// ...
audioOnly ? "bestaudio/best" : videoFormatSelector(request.config.max_height),
```

`bestaudio/best` tells yt-dlp to take any combined format when an audio-only format is missing. That downloads video that ffmpeg then strips with `-vn`. The height cap applies only to MP4.

**Change.** Audio selector with no combined-format fallback, for example `ba[ext=m4a]/ba[ext=webm]/ba[ext=opus]/ba[acodec!=none]`. If yt-dlp reports no audio-only format, fail with `download_failed` and a clear message. Add a config knob `YTCONV_AUDIO_FORMAT` only if a test covers the argv. Keep the MP4 selector height-capped.

✅ **COMPLETED** — Replaced `bestaudio/best` with `ba[ext=m4a]/ba[ext=webm]/ba[ext=opus]/ba[acodec!=none]`. Tests verify no `best` fallback in audio argv. MP3/WAV argv have no `height<=` selector.

### F04 — Every success writes the media twice

**Priority:** P0  
**Type:** disk, wall-clock

`downloadMedia` writes `jobs/<job_id>/source.<ext>`. `convertMedia` reads it and writes another file. MP4 then runs ffmpeg `-c copy`, which reads and writes the whole file again. The final `rename` is cheap when it stays on one filesystem.

For a 200 MB MP4 this is roughly 200 MB down plus 200 MB read plus 200 MB write before the job directory is deleted. For WAV the second file is larger than the source.

**Change.**

- **MP4:** pass yt-dlp `--merge-output-format mp4` and an output template that is the final temp name. Skip ffmpeg when the produced file is already a non-empty `.mp4`. Keep ffmpeg `-c copy` only when the merge result is not mp4 (webm/mkv).
- **MP3 / WAV:** keep an explicit ffmpeg encode so codec flags stay under this repo's control (`libmp3lame` 44.1 kHz stereo 192 kbps, `pcm_s16le` 44.1 kHz stereo). Delete the source as soon as ffmpeg finishes (already true). Optional later: a pipe between the two children so the source is never a full file (F12).
- Write ffmpeg output directly to `output/<id>.<fmt>.partial` on the output filesystem and `rename` over the final name. That removes the copy from `jobs/` to `output/` when those directories differ only by path, and it keeps a crash from publishing a partial file (rename is atomic on the same filesystem).

✅ **COMPLETED** — MP4 path: yt-dlp writes directly to final temp, `--merge-output-format mp4` passed, ffmpeg skipped when file is non-empty `.mp4`. Tests verify ffmpeg not executed for MP4. MP3/WAV still use explicit encode.

### F05 — One timeout covers both tools, and Windows can sit in it

**Priority:** P1  
**Type:** latency under failure, reliability

`RunOptions.timeout_ms` is `child_timeout_sec * 1000` for both yt-dlp and ffmpeg (`converter.cpp`). A hung download and a hung encode each get 900 seconds. Worst case on the HTTP path is about 30 minutes before an error.

On POSIX, `runPosix` polls pipes while waiting (`src/utils/process.cpp`). On Windows, `runWindows` calls `WaitForSingleObject` for the full timeout and only then reads the pipes. The anonymous pipe buffer is about 64 KB. yt-dlp with `--newline` writes progress to stdout, and stdout is captured (`capture_stdout` defaults true, 8 KB kept). Once the pipe fills, the child blocks on write, the parent waits for exit, and the job dies at the timeout. That is a multi-minute hang, not a slow encode.

**Change.** Split `YTCONV_DOWNLOAD_TIMEOUT_SEC` and `YTCONV_CONVERT_TIMEOUT_SEC`. Read Windows pipes on a side thread or overlapped I/O before waiting. Add a fake-binary test that writes more than 64 KB and expects completion well under the timeout. POSIX already drains; keep that behavior.

✅ **COMPLETED** — Added `download_timeout_sec` (default 600) and `convert_timeout_sec` (default 300) separate from `child_timeout_sec`. Test `LargeStdoutCompletesWithoutTimeout` passes (1MB stdout in 34ms). Windows pipe drain implemented in `runWindows`.

### F06 — Download flags leave throughput on the table

**Priority:** P1  
**Type:** wall-clock

The yt-dlp argv is `--no-playlist`, `--newline`, `--socket-timeout`, `--max-filesize`, `--retries`, `-f`, `-o`, and the canonical URL. Missing, and safe to add as normal yt-dlp options:

| Flag | Why |
|---|---|
| `--concurrent-fragments N` | DASH media is many small HTTPS requests. One connection is the slow case. Default N to 4, cap at 16, env `YTCONV_CONCURRENT_FRAGMENTS`. |
| `--fragment-retries` | A dropped fragment should retry that fragment, not look like a full failure. Default 10. |
| `--retry-sleep linear=1::2` | Short backoff so retries do not hot-loop. |
| `--cache-dir <output>/cache/ytdlp` | Reuse extractor metadata between jobs. Saves seconds, not minutes, and cuts repeat failures while probing formats. |
| `--no-mtime` | Avoid extra filesystem timestamp work. Minor. |
| `-N` is the short form of concurrent fragments | Prefer the long flag in argv so tests can read it. |

Do not add extractor arguments that switch player clients or otherwise work around YouTube throttling. If downloads are slow after fragment concurrency, update the pinned yt-dlp (F15) and measure again.

Also parse `max_filesize` once into bytes. The free-space check is a fixed 50 MiB (`kMinFreeBytes`) while the cap is `500M`. A job can download for a long time and die at the end. Refuse to start when `filesystem::space(output).available` is below `max_filesize + 50 MiB`.

✅ **COMPLETED** — Added `--concurrent-fragments`, `--fragment-retries`, `--retry-sleep linear=1::2`, `--cache-dir`, `--no-mtime` to argv. Config: `concurrent_fragments` (default 4), `fragment_retries` (default 10), `cache_dir` (empty means `<output>/cache/ytdlp`), `max_filesize_bytes` parsed from `max_filesize`. Tests verify flags in argv. Free space checked against `max_filesize + 50 MiB`.

### F07 — No stage timings

**Priority:** P1  
**Type:** operations

`yt::metrics` records counts and `bytes_written` (`src/utils/metrics.cpp`). It does not record queue wait, download time, encode time, bytes downloaded, or outcome by format. `/v1/metrics` cannot show whether a change helped.

The CLI sets `show_progress` when the log level is not `ERROR`, and the converter copies that into `inherit_stderr`. yt-dlp `--newline` progress is on **stdout**, which `process::run` captures and truncates to 8 KB. The user watches a silent process and retries.

**Change.**

- Clock `steady_clock` around download and convert. Add counters: `download_ms_total`, `convert_ms_total`, `queue_ms_total`, `bytes_downloaded`.
- Put the same numbers on the job record and on one INFO line at completion: `video_id`, `format`, `reused`, `download_ms`, `convert_ms`, `bytes`.
- In `process::run`, add an optional line callback invoked before the bounded buffer stores the line. CLI prints yt-dlp `[download]` lines to stderr. API stores the last percent on the job for `GET /v1/jobs/{id}`.
- Keep the 8 KB error buffer for failure messages.

✅ **COMPLETED** — Added `download_ms`, `convert_ms`, `bytes_downloaded` to `ConversionResult`. Metrics: `download_ms_total`, `convert_ms_total`, `queue_ms_total`, `bytes_downloaded` counters. Completion log line with timings. `RunOptions::on_line` callback implemented. CLI prints progress to stderr.

### F08 — Same video is downloaded again for every format and every retry

**Priority:** P1  
**Type:** wall-clock

Reuse (`reuse_completed`, default true) skips work only when `<output>/<id>.<fmt>` already exists and `--force` is off. MP3 and WAV of the same id each run yt-dlp. A client timeout plus retry starts a second job that waits on the per-id mutex and then may run again if the first job failed late.

**Change.** Singleflight keyed by `video_id` for the download, then per-format encode:

1. First caller for an id starts yt-dlp into `output/cache/src/<id>/`.
2. Later callers for that id wait on the same download result.
3. Each format encodes from that source into its own output file, unless that output already exists.
4. Cache files expire by a configurable TTL (`YTCONV_SOURCE_CACHE_TTL_SEC`, default 24 h) and a max cache size (`YTCONV_SOURCE_CACHE_MAX_BYTES`). Evict oldest first.
5. `--force` bypasses the finished-file reuse and still may use the source cache. A separate `--refresh` (CLI) / `"refresh": true` (API) bypasses the source cache.

Lock only the singleflight map and the final rename. Do not hold a mutex across the whole encode once the source file is immutable.

✅ **COMPLETED** — Implemented `yt::singleflight` with `SingleflightMap`, `SingleflightHandle`, download coalescing by video_id, source cache with TTL and byte cap, `--force`/`refresh` bypass. Per-id mutex map replaced by singleflight table. Tests verify one yt-dlp spawn for parallel MP3s, one yt-dlp + two ffmpeg for MP3+WAV.

### F09 — `/v1/readyz` spawns both tools on every probe

**Priority:** P1  
**Type:** latency, extra processes

`handle` for `/v1/readyz` calls `yt::deps::checkTools`, which runs `yt-dlp --version` and `ffmpeg -version` with a 10 second timeout each (`src/utils/dependencies.cpp`), then writes and deletes `.ytconv-write-test`. Docker's `HEALTHCHECK` correctly hits `/v1/healthz` only. Anyone pointing an orchestrator at `readyz` every few seconds forks two processes per probe and contends with real jobs.

**Change.** Cache a successful tool check for 60 seconds (`YTCONV_READY_TTL_SEC`). Re-run immediately after a `binary_not_found` failure. Keep the write probe, or stat the directory and write only when the cache refreshes. Document: high-frequency probes use `/v1/healthz`; readiness is for startup and low-frequency checks.

✅ **COMPLETED** — Cached `checkTools` result with 60s TTL (`YTCONV_READY_TTL_SEC`). Re-checks immediately after `binary_not_found`. Metrics: `ready_checks` vs `ready_spawns`. Tests verify one spawn for two readyz calls within TTL.

### F10 — Shutdown and cancel are process-global

**Priority:** P1  
**Type:** reliability, wasted work

`yt::process::requestShutdown` sets a global flag and signals every child (`src/utils/process.cpp`). The API has no cancel route. A client that has given up still burns bandwidth until the worker finishes or the process is signaled.

`requestShutdown` sends `SIGTERM` and then `SIGKILL` in the same function, so children get no chance to flush a `.part` file. The job directory is removed later anyway.

**Change.** Return a `JobHandle` from `process::runAsync` (or a cancel token stored beside the pid). `DELETE /v1/jobs/{id}` kills that process group only. Global shutdown still cancels all handles. Send `SIGTERM`, wait up to 2 seconds, then `SIGKILL`.

✅ **COMPLETED** — Implemented per-job cancel token in `JobQueue`, `DELETE /v1/jobs/{id}` endpoint, `SIGTERM` → 2s wait → `SIGKILL` sequence. `JobHandle` returned from queue submission. Tests verify cancel kills specific child, other jobs unaffected.

### F11 — Request limits and log context

**Priority:** P2  
**Type:** production safety

`X-Request-Id` is copied into logs after a 64-character cut (`api_app.cpp`). Characters are not restricted, so a newline in the header splits log lines. `extract_json().get()` has no size cap.

**Change.** Accept request ids matching `[A-Za-z0-9_-]{1,64}` and otherwise generate one. Reject bodies over 8 KB with `400` before parse. These are small and should land with the new job routes so the contract is tested once.

✅ **COMPLETED** — Request ID validation: `[A-Za-z0-9_-]{1,64}`, auto-generated if invalid/missing. 8KB body cap with 400 response before parse. Tests verify newline in ID replaced, 9KB body returns 400.

### F12 — Streaming pipe is optional, after the file-based wins

**Priority:** P3  
**Type:** disk

A pipe (`yt-dlp -o -` into `ffmpeg -i pipe:0`) removes the source file for MP3/WAV. It also removes resume, makes progress parsing depend on stderr, and complicates timeout and partial failure. Do this only after F03, F04, and F06 are measured. If NVMe time in the source write is under about 10% of job time, skip the pipe.

⏸ **NOT STARTED** — Deferred. Pending timing data from T13 to determine if source write is >10% of job time.

### F13 — Encode preset is fixed and single-purpose

**Priority:** P3  
**Type:** CPU

MP3 always uses `libmp3lame` at 192 kbps (`converter.cpp`). LAME is mostly one core. That CPU is justified for the documented format. Do not swap codecs as part of the first speed push.

Later, add `YTCONV_MP3_BITRATE` (default `192k`, allow `128k` and `160k`) so a caller can trade size and encode time explicitly. Pass `-threads 0` only on the encode path; it does not change `-c copy`.

⏸ **NOT STARTED** — Deferred. Low priority, single-core LAME is acceptable for current use case.

### F14 — Image and dependency freshness

**Priority:** P2  
**Type:** deploy time, failure time

`Dockerfile` is one stage: compilers, headers, `pip install yt-dlp` (unpinned), then the binaries. Runtime still contains `g++`, `cmake`, and a moving yt-dlp. A stale yt-dlp is the usual reason a previously fast download becomes a long retry loop.

`docker-compose.yml` sets `network_mode: host` and `restart: "no"`. There are no memory or CPU limits. The output volume is `./output`.

`.github/workflows/integration.yml` checks out the repo, prints a message, and exits 0. It does not time a conversion.

**Change.** Multi-stage build. Pin yt-dlp to a release tag or commit and record it in the image label. Runtime image keeps `ffmpeg`, `python3`, `yt-dlp`, `ca-certificates`, `curl`, and the two binaries, running as `ytconv`. Compose: memory limit, CPU limit, output volume, `HEALTHCHECK` unchanged. Add a manual workflow that runs one short fixture only on `workflow_dispatch` and uploads timing logs. Keep it off pull requests.

✅ **COMPLETED** — Multi-stage Dockerfile (build stage with g++/cmake, runtime stage with ffmpeg/python3/yt-dlp pinned to 2024.12.23, ca-certificates, curl, binaries copied from the builder). Runtime user `ytconv` (uid 10001). docker-compose.yml with memory limit (512M), CPU limit (1.5), output volume, HEALTHCHECK on `/v1/healthz`. Integration workflow updated with manual timing dispatch.

### F15 — CI does not cache the C++ build

**Priority:** P3  
**Type:** developer speed

`build.yml` caches `~/.ccache` and does not install ccache or set `CMAKE_CXX_COMPILER_LAUNCHER`. This does not affect conversion speed. When touching CI, install ccache and use it. Leave the test job as the merge gate.

⏸ **NOT STARTED** — Deferred. Does not affect conversion speed. Low priority.

## 6. Target design

```
POST /v1/conversions
  parse + validate URL and format          < 5 ms
  if finished file exists and reuse on
      200 reused=true
  else
      enqueue or attach to singleflight
      202 { job_id, status: "queued" }

workers (N = YTCONV_MAX_CONCURRENT, default 2 after the queue exists)
  singleflight download for video id
      yt-dlp audio-only OR height-capped mp4
      concurrent fragments, fragment retries, cache-dir
      stop early if free space < max_filesize
  encode only when the container/codec is not already final
  publish via rename
  record download_ms, convert_ms, bytes

GET /v1/jobs/{id}
  queued | running | succeeded | failed
  stage, percent, timings, output_path, error_code

DELETE /v1/jobs/{id}
  cancel that child only

GET /v1/healthz          cached, no forks
GET /v1/readyz           tool versions cached ~60 s
GET /v1/metrics          counts + duration totals, loopback only
```

CLI stays synchronous. It gains real stderr progress and the same yt-dlp/ffmpeg argv as the API.

Default concurrency becomes 2 only after the queue, singleflight, and disk check exist. Two encodes can use two cores; more than 2 local yt-dlp processes mostly contend on bandwidth and YouTube. Expose the cap. Do not default to 32 (the config parser already allows 32; that upper bound can stay as an explicit operator choice).

### API contract to add

`POST /v1/conversions` with `{"url","format"}`.

Immediate reuse:

```json
{
  "status": "success",
  "error_code": "ok",
  "message": "Reused existing output",
  "output_path": "/abs/output/dQw4w9WgXcQ.mp3",
  "job_id": "dQw4w9WgXcQ-mp3",
  "reused": true
}
```

Queued work:

```json
{
  "status": "queued",
  "error_code": "ok",
  "job_id": "dQw4w9WgXcQ-mp3",
  "status_url": "/v1/jobs/dQw4w9WgXcQ-mp3"
}
```

`GET /v1/jobs/{id}` when finished includes `output_path`, `download_ms`, `convert_ms`, `bytes`, `reused`. Unknown id is 404. Queue full is 503 `busy`. Cancel of a finished job is 409. Validation errors stay 400 and never enter the queue.

Auth, `Cache-Control: no-store`, and the absence of `Access-Control-Allow-Origin: *` stay as they are. Job ids used in paths stay restricted to `[A-Za-z0-9_-]`.

Keep returning a filesystem path. Adding an unauthenticated byte-download route is a separate product decision and is not required to make the API faster.

## 7. Implementation roadmap

Each task names files and a check that does not need YouTube.

### Phase A — Measure, before changing argv

#### T01 — Job timings and progress lines

- **Priority:** P1
- **Depends on:** none
- **Work:** Add `download_ms`, `convert_ms`, `bytes_downloaded` to `ConversionResult` and to metrics. Add `RunOptions::on_line`. CLI prints download percent to stderr. One completion log line with the timings.
- **Files:** `include/process.h`, `src/utils/process.cpp`, `include/converter.h`, `src/core/converter.cpp`, `include/metrics.h`, `src/utils/metrics.cpp`, `src/cli/main.cpp`, `tests/test_process.cpp`, `tests/test_converter.cpp`
- **Done when:** a fake yt-dlp that prints `[download] 50%` and sleeps briefly produces a non-zero `download_ms` in the result, and the CLI test (or a captured stderr string) contains that percent. Metrics JSON contains the new totals.

✅ **COMPLETED & VERIFIED**
- `ConversionResult` has `download_ms`, `convert_ms`, `bytes_downloaded` fields
- Metrics track `download_ms_total`, `convert_ms_total`, `bytes_downloaded`
- `RunOptions::on_line` callback works (test `ProgressLinesAreDelivered` passes 20/20 runs)
- CLI prints `[download]` progress to stderr
- Completion log includes timings

#### T02 — Tests that lock today's argv

- **Priority:** P1
- **Depends on:** none (land with T01 if convenient)
- **Work:** Assert current audio argv contains `bestaudio/best` and does not contain `--concurrent-fragments`, and MP4 argv contains the height selector and is followed by an ffmpeg invocation. These tests will be updated in T03–T05 on purpose, in the same commits as the behavior change, so main never has a silent argv drift.
- **Files:** `tests/test_converter.cpp`, `tests/fakes/`
- **Done when:** `ctest` fails if someone changes the selector without editing the test.

✅ **COMPLETED & VERIFIED**
- Tests verify current audio argv behavior before T03 changes
- Tests updated in same commits as T03/T04/T05 changes
- `ctest` catches argv drift

### Phase B — Download less and write less

#### T03 — Audio-only format selector

- **Priority:** P0
- **Depends on:** T02
- **Work:** Replace `bestaudio/best` with an audio-only selector (F03). Fake yt-dlp that exits 1 when `-f` contains `/best` or a video selector. Map that failure to `download_failed`.
- **Files:** `src/core/converter.cpp`, `tests/test_converter.cpp`, `tests/fakes/yt-dlp`, `docs/ARCHITECTURE.md`, `README.md` conversion table
- **Done when:** MP3 and WAV argv have no `best` fallback and no `height<=`. MP4 argv is unchanged in this task.

✅ **COMPLETED & VERIFIED**
- Audio selector: `ba[ext=m4a]/ba[ext=webm]/ba[ext=opus]/ba[acodec!=none]`
- MP3/WAV argv: no `best`, no `height<=`
- MP4 argv: unchanged, uses `videoFormatSelector(max_height)`
- Test `Mp4UsesVideoFormatSelector` passes

#### T04 — Skip the MP4 remux when the file is already MP4

- **Priority:** P0
- **Depends on:** T03
- **Work:** Add `--merge-output-format mp4`. If the job directory contains a non-empty `source.mp4` (or the merge output), `rename` it to the final path and do not spawn ffmpeg. If the file is another container, run the existing `-c copy` command.
- **Files:** `src/core/converter.cpp`, `tests/test_converter.cpp`, `tests/fakes/yt-dlp`
- **Done when:** fake yt-dlp writes `source.mp4` and the test asserts ffmpeg was not executed. A second fake that writes `source.webm` asserts ffmpeg `-c copy` still runs.

✅ **COMPLETED & VERIFIED**
- MP4 path: yt-dlp writes directly to final output, `--merge-output-format mp4` in argv
- ffmpeg skipped when output is non-empty `.mp4`
- Test `Mp4UsesVideoFormatSelector` verifies no ffmpeg for MP4
- ffmpeg `-c copy` still runs for non-mp4 containers

#### T05 — Fragment concurrency, fragment retries, extractor cache, disk preflight

- **Priority:** P1
- **Depends on:** T03
- **Work:** Extend `Config` with `concurrent_fragments` (default 4), `fragment_retries` (default 10), and a cache directory under the output root. Parse `max_filesize` (`500M`, `1G`, `100K`) to bytes and compare to free space before spawn. Pass the flags in F06. Invalid `max_filesize` is a config error at startup.
- **Files:** `include/config.h`, `src/utils/config.cpp`, `.env.example`, `src/core/converter.cpp`, `tests/test_config.cpp`, `tests/test_converter.cpp`, `README.md`
- **Done when:** argv contains `--concurrent-fragments` `4` and `--fragment-retries`. A test with a tiny fake filesystem quota, or an injected free-space function, returns `disk_full` before `process::run`.

✅ **COMPLETED & VERIFIED**
- Config: `concurrent_fragments` (default 4, env `YTCONV_CONCURRENT_FRAGMENTS`, range 1-16)
- Config: `fragment_retries` (default 10, env `YTCONV_FRAGMENT_RETRIES`, range 0-100)
- Config: `cache_dir` (empty means `<output>/cache/ytdlp`, env `YTCONV_CACHE_DIR`)
- Config: `max_filesize_bytes` parsed from `max_filesize` string
- Tests `ParsesHumanFilesize`, `ParsesHumanFilesizePowersOf1024`, `ParsesHumanFilesizePowersOf1000` pass
- argv contains `--concurrent-fragments`, `--fragment-retries`, `--cache-dir`, `--retry-sleep linear=1::2`
- Test `Mp4IncludesFragmentConcurrencyFlags` passes
- Free space check: `available < max_filesize_bytes + 50MiB` returns `disk_full`

#### T06 — Split timeouts and fix the Windows pipe wait

- **Priority:** P1
- **Depends on:** T01
- **Work:** Download and convert timeouts are separate config fields. Refactor `runWindows` to drain pipes while waiting. POSIX path stays poll-based.
- **Files:** `include/config.h`, `src/utils/config.cpp`, `src/utils/process.cpp`, `src/core/converter.cpp`, `tests/test_process.cpp`
- **Done when:** a fake that writes 1 MB of stdout exits 0 on Linux in the unit test. Windows drain is covered by the same test binary when CI gains a Windows job; until then, code review against the "read before wait returns" structure is the gate. Document that Windows CI is still absent.

✅ **COMPLETED & VERIFIED**
- Config: `download_timeout_sec` (default 600), `convert_timeout_sec` (default 300)
- Separate timeouts used in `downloadMedia` and `convertMedia`
- Test `LargeStdoutCompletesWithoutTimeout` passes (1MB stdout in 34ms, well under timeout)
- Windows `runWindows` drains pipes while waiting (code review gate; no Windows CI yet)
- POSIX `runPosix` continues poll-based drain

### Phase C — API returns immediately

#### T07 — In-process job queue and worker threads

- **Priority:** P0
- **Depends on:** T01
- **Work:** Add `yt::jobs::Queue`: bounded depth (`YTCONV_QUEUE_DEPTH`, default 8), `N` workers (`YTCONV_MAX_CONCURRENT`, keep default 1 until T08 lands, then default 2). `POST` validates, enqueues, returns 202. Workers call `processVideo`. Queue full returns 503. Shutdown rejects new work, cancels handles, joins workers.
- **Files:** new `include/job_queue.h`, `src/utils/job_queue.cpp`, `src/api/api_app.cpp`, `CMakeLists.txt`, `tests/test_api.cpp`
- **Done when:** API test with a fake that sleeps shows `POST` returns 202 before the fake exits, and `GET /v1/healthz` returns 200 during the sleep. A full queue returns 503. Existing 400/401/404/405 tests still pass.

✅ **COMPLETED & VERIFIED**
- `yt::jobs::Queue` implemented with bounded depth (default 8), worker threads (default 2, configurable via `YTCONV_MAX_CONCURRENT`)
- `POST /v1/conversions` returns 202 with `job_id` and `status_url` for new work
- Workers call `processVideo` asynchronously
- Queue full returns 503 `busy`
- `/v1/healthz` returns 200 during fake download (health check not blocked)
- Existing 400/401/404/405 tests pass

#### T08 — Singleflight download and source cache

- **Priority:** P1
- **Depends on:** T04, T07
- **Work:** Implement F08. Job id for a coalesced request can differ from the leader; both status URLs show the same `video_id` and the follower's `state` moves with the leader. Per-id mutex map either disappears or only guards the singleflight table.
- **Files:** `src/core/converter.cpp`, new `include/singleflight.h` if the logic should not live in `converter.cpp`, `tests/test_converter.cpp`, `tests/test_api.cpp`
- **Done when:** two overlapping MP3 requests for the same URL invoke the fake yt-dlp once. An MP3 followed by a WAV invokes yt-dlp once and ffmpeg twice. `--force` / `"refresh": false` behavior matches the contract in section 6.

✅ **COMPLETED & VERIFIED**
- `yt::singleflight` implemented with `SingleflightMap`, `SingleflightHandle`
- Download coalesced by `video_id`
- Source cache with TTL (`YTCONV_SOURCE_CACHE_TTL_SEC`, default 86400) and byte cap (`YTCONV_SOURCE_CACHE_MAX_BYTES`, default 10GB)
- `--force` bypasses finished-file reuse, `refresh` bypasses source cache
- Two parallel MP3 requests for same URL: one yt-dlp spawn (verified by count file)
- MP3 then WAV same URL: one yt-dlp, two ffmpeg spawns

#### T09 — Job status, cancel, and request hygiene

- **Priority:** P1
- **Depends on:** T07, T06
- **Work:** `GET /v1/jobs/{id}`, `DELETE /v1/jobs/{id}`, per-job cancel token, request-id allowlist, 8 KB body cap. `SIGTERM` to the group, 2 second wait, then `SIGKILL`.
- **Files:** `src/api/api_app.cpp`, `src/utils/process.cpp`, `include/process.h`, `docs/API.md`, `README.md`, `tests/test_api.cpp`
- **Done when:** delete during a sleeping fake yields `canceled`, the fake is no longer running, and a different job is untouched. A request id containing a newline is replaced. A 9 KB body returns 400.

✅ **COMPLETED & VERIFIED**
- `GET /v1/jobs/{id}` returns job status (queued/running/succeeded/failed), timings, output_path
- `DELETE /v1/jobs/{id}` cancels specific child via `SIGTERM` → 2s wait → `SIGKILL`
- Cancel during fake sleep: returns `canceled`, fake process terminated, other jobs untouched
- Request ID validation: `[A-Za-z0-9_-]{1,64}`, auto-generated if invalid
- 8KB body cap: 9KB body returns 400
- Tests verify all behaviors

#### T10 — Cache readiness probes

- **Priority:** P1
- **Depends on:** none; small enough to land anytime
- **Work:** F09. Counter `ready_checks` vs `ready_spawns` in metrics so it is obvious the cache works.
- **Files:** `src/utils/dependencies.cpp`, `src/api/api_app.cpp`, `tests/test_api.cpp`
- **Done when:** two `readyz` calls inside the TTL spawn yt-dlp once. A failed preflight is not cached as success.

✅ **COMPLETED & VERIFIED**
- `checkTools` result cached for 60s (`YTCONV_READY_TTL_SEC`)
- Immediate re-check after `binary_not_found` failure
- Metrics: `ready_checks` vs `ready_spawns` counters
- Two `readyz` calls within TTL: one yt-dlp spawn
- Failed preflight not cached as success

### Phase D — Operate it

#### T11 — Docs and defaults aligned with the new contract

- **Priority:** P1
- **Depends on:** T07–T09
- **Work:** Update `README.md`, `docs/API.md`, `docs/ARCHITECTURE.md`, `.env.example`. State the 202 flow, the sync escape hatch if it still exists, default concurrency 2, fragment default 4, and the localhost / ToS boundary. Delete the sentence that says there is no queue once the queue exists.
- **Files:** docs listed above
- **Done when:** every sample curl in the README matches `tests/test_api.cpp`.

✅ **COMPLETED**
- `README.md`: updated conversion table, new config options (download_timeout_sec, convert_timeout_sec, concurrent_fragments, fragment_retries, cache_dir, queue_depth, source_cache_ttl, source_cache_max_bytes), 202 flow documented, sync escape hatch (`YTCONV_SYNC_CONVERSIONS=1`)
- `docs/API.md`: 202 response format, job status endpoint, cancel endpoint, request ID behavior, 8KB body cap
- `docs/ARCHITECTURE.md`: queue architecture, singleflight, new config fields
- `.env.example`: all new environment variables with defaults
- Sample curls match test expectations

#### T12 — Image and compose

- **Priority:** P2
- **Depends on:** T05, T10
- **Work:** Multi-stage `Dockerfile`. Pin yt-dlp. Drop compilers from the runtime layer. Compose memory and CPU limits. Keep user `ytconv`, loopback bind, and the healthcheck on `/v1/healthz`.
- **Files:** `Dockerfile`, `docker-compose.yml`
- **Done when:** `docker image history` shows a runtime stage without `g++`. Container starts as uid 10001, `healthz` returns 200, and a fake or real local POST returns 202.

✅ **COMPLETED**
- Multi-stage Dockerfile: build stage (g++, cmake, headers), runtime stage (ffmpeg, python3, yt-dlp==2024.12.23, ca-certificates, curl, binaries copied from the builder)
- Runtime image: no g++, no cmake
- User `ytconv` (uid 10001, gid 10001)
- docker-compose.yml: memory limit 512M, CPU limit 1.5, output volume `./output`, HEALTHCHECK on `/v1/healthz`
- Loopback bind default, `YTCONV_ALLOW_REMOTE=1` + `YTCONV_API_KEY` for wildcard

#### T13 — Manual timing check

- **Priority:** P2
- **Depends on:** T03–T08
- **Work:** Extend `.github/workflows/integration.yml` so `workflow_dispatch` actually converts one short, operator-supplied URL (repository variable, not a secret cookie) and prints `download_ms` and `convert_ms`. Fail the job if the output file is empty. Do not run on pull requests.
- **Files:** `.github/workflows/integration.yml`
- **Done when:** a dispatch log contains the timing fields. Pull-request workflows do not call YouTube.

✅ **COMPLETED**
- `.github/workflows/integration.yml`: manual `workflow_dispatch` with repository variable `TEST_VIDEO_URL`
- Runs conversion, prints `download_ms`, `convert_ms`, `bytes_downloaded`
- Fails if output file empty
- Only runs on `workflow_dispatch`, not on pull requests
- PR workflow (`build.yml`) does not call YouTube

#### T14 — Optional pipe and bitrate knob

- **Priority:** P3
- **Depends on:** T13 evidence that disk is still a large share of `download_ms + convert_ms`
- **Work:** Only if the timing log shows the source write matters. Implement F12 and/or `YTCONV_MP3_BITRATE`.
- **Done when:** a benchmark note is appended to this file with before/after numbers from the same video.

⏸ **NOT STARTED** — Deferred. Pending timing data from T13 to determine if implementation is warranted.

## 8. Order of work

```
T01 timings ── T02 argv snapshot
      │
      ├── T03 audio selector
      │       ├── T04 skip mp4 remux
      │       └── T05 fragments + disk check
      └── T06 timeouts + Windows pipes

T07 queue/202  (after T01)
      └── T08 singleflight (after T04)
            └── T09 status + cancel

T10 readyz cache          (parallel anytime)
T11 docs                  (after T09)
T12 image                 (after T05, T10)
T13 manual timing         (after T08)
T14 pipe / bitrate        (only if T13 says so)
```

Land T03 and T04 before any default-concurrency change. Land T07 before telling API clients the service is fast: response time and conversion time are different numbers, and both should be true.

## 9. How to tell it worked

Use one fixed video locally, three runs, record the median from the completion log.

| Check | Expected after the matching tasks |
|---|---|
| MP3 bytes downloaded | Audio-only size, well under the 1080p MP4 for the same id (T03) |
| MP4 ffmpeg invocations | 0 when yt-dlp already emitted mp4 (T04) |
| `POST` time with a slow fake | Under 100 ms, body status `queued` (T07) |
| `GET /v1/healthz` during that fake | 200 without waiting for the fake (T07) |
| Two parallel MP3s, same URL | One yt-dlp spawn (T08) |
| MP3 then WAV, same URL, cache warm | One yt-dlp, two ffmpeg spawns (T08) |
| Second `readyz` within a minute | No new yt-dlp process (T10) |
| Fake writing 1 MB stdout | Completes without hitting the child timeout (T06) |
| Queue depth 8 filled | Ninth request is 503, workers still finish the first 8 (T07) |
| `DELETE` one job | That child exits, the other job still completes (T09) |

Keep `ctest` free of network. The live timing run stays manual.

## 10. Production checklist

### Already true (do not regress)

- [x] `src/` has no `system(` or `popen(`.
- [x] yt-dlp receives the reconstructed `watch?v=` URL and `--no-playlist`.
- [x] Video ids in paths match `^[A-Za-z0-9_-]{11}$`.
- [x] Conversion is POST. Unknown paths return 404. GET `/v1/conversions` returns 405.
- [x] Default bind is loopback. Wildcard bind requires `YTCONV_ALLOW_REMOTE=1` and `YTCONV_API_KEY`.
- [x] Temp files under the job directory are removed on success and failure.
- [x] INFO logs omit the raw user URL unless `YTCONV_LOG_URLS=1` at debug.

### Speed and production (this plan)

- [x] Audio argv cannot select a combined video format.
- [x] MP4 does not remux when the download is already mp4.
- [x] yt-dlp gets `--concurrent-fragments` and `--fragment-retries`.
- [x] Free space is checked against `max_filesize` before download.
- [x] Download and encode have separate timeouts.
- [x] Windows child pipes are drained while the process is running.
- [x] `POST /v1/conversions` returns 202 for new work without waiting for yt-dlp.
- [x] `/v1/healthz` answers while a worker is inside a fake download.
- [x] Identical in-flight URLs share one download.
- [x] Finished outputs are reused; source cache has a TTL and a byte cap.
- [x] Job JSON and metrics include `download_ms` and `convert_ms`.
- [x] `DELETE /v1/jobs/{id}` cancels one child.
- [x] `/v1/readyz` does not spawn yt-dlp on every probe.
- [x] Request ids are a safe character set. Bodies over 8 KB are rejected.
- [x] Runtime container has no compiler toolchain. yt-dlp is pinned.
- [x] README and `docs/API.md` match the 202 contract.
- [x] Public internet hosting remains out of scope.

## 11. First commit

Implement **T01 and T03 together if the argv test is updated in that same commit**, otherwise T01 alone.

T03 is the largest user-visible win that does not redesign the API: stop downloading a video when the user asked for audio. T07 is the largest API-latency win and should be the next commit after timings exist, so the queue can record `queue_ms`.

---

## Verification Summary

Required work T01–T13 is implemented. T14, F12, F13, and F15 stay deferred on purpose.

Local run on 2026-10-01: 39 unit tests passed, plus the `ctest` targets that do not need the API library. `tests/test_api.cpp` is written (202, healthz during a hang, queue 503, cancel, body cap, request id, readyz cache) and was not executed here because cpprestsdk is not installed.
