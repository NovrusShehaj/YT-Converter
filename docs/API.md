# API

The API is a localhost HTTP service. It is not a public converter and does not serve media bytes.

## Start

```sh
# Conscious unauthenticated localhost (tests / personal use)
./yt2mp3-api --allow-unauthenticated-localhost

# Or require a header
YTCONV_API_KEY='a-long-secret' ./yt2mp3-api
```

Default listen URL: `http://127.0.0.1:8080`.

Remote bind (`0.0.0.0` or `::`) is refused unless `YTCONV_ALLOW_REMOTE=1` and `YTCONV_API_KEY` is set.

## Authorization

When `YTCONV_API_KEY` is set, every service route requires `X-Api-Key: <key>`:
`POST /v1/conversions`, `GET /v1/jobs/{id}`, and `DELETE /v1/jobs/{id}`. The key is checked before
any job lookup, so a missing or wrong key returns `401 unauthorized` whether or not the job exists,
and the response never includes job fields. With the correct key the routes behave normally
(200/404/409).

The single service key authorizes all service jobs. There are no user accounts and no per-user job
ownership; random job IDs are identifiers, not credentials. `/v1/healthz` and `/v1/readyz` stay
public, and `/v1/metrics` is loopback-only. Without a key, the API only starts in the explicit
`--allow-unauthenticated-localhost` mode, where all routes are open to local clients.

## Convert

`POST /v1/conversions`

```json
{"url":"https://www.youtube.com/watch?v=dQw4w9WgXcQ","format":"mp3"}
```

Required header when an API key is configured: `X-Api-Key`.

### Immediate reuse (output exists)

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

### Queued work

```json
{
  "status": "queued",
  "error_code": "ok",
  "job_id": "dQw4w9WgXcQ-mp3",
  "status_url": "/v1/jobs/dQw4w9WgXcQ-mp3"
}
```

When the output file already exists and completed-output reuse applies, the API returns 200 with `reused: true`. Otherwise it returns 202 `queued` and the conversion runs asynchronously. The reuse `job_id` is informational; no job record is created for it.

### Reuse, `force`, and `refresh`

Optional boolean body fields `force` and `refresh` select the policy. One rule is used everywhere
(HTTP fast path, queue, converter, CLI): completed-output reuse requires `YTCONV_REUSE_COMPLETED`
(default on), `force=false`, `refresh=false`, and a usable output file.

| Completed-output reuse enabled | `force` | `refresh` | Behavior |
|---|---|---|---|
| Yes | No | No | Reuse a valid completed output; otherwise encode from a fresh cached source or a new download |
| Yes | Yes | No | Re-encode and replace the output; a fresh cached source is allowed |
| Any | Any | Yes | Download a new source generation, then replace the output |
| No | Any | No | Encode and publish again; a fresh cached source is allowed |

`reused: true` means a completed output file satisfied the request, never merely that download
bytes were shared. Replacements keep the previous output if the download or encode fails.

Sharing and ordering rules:

- Requests share an operation only with the same video, format, and policy class (reuse,
  replace, refresh). A `refresh` shares only a refresh operation that has not started, and only
  a download that started after the refresh was admitted, so it never reuses older bytes.
- Writers to one output are serialized. Before publishing, a writer skips publication (and
  reports the existing output with `reused: true`) when the output was built from a newer source
  generation, or, for non-refresh replacements, when it was republished after this request was
  admitted. An older operation therefore never overwrites a later refresh.
- A job canceled before publication never creates new output.

### Job status

`GET /v1/jobs/{job_id}` returns `queued`, `running`, `succeeded`, `failed`, or `canceled`. A finished job includes `output_path`, `download_ms`, `convert_ms`, `bytes`, and `reused`. `stage` and `percent` describe in-flight work.

```json
{
  "job_id": "dQw4w9WgXcQ-mp3-1a2b3c4d",
  "video_id": "dQw4w9WgXcQ",
  "status": "succeeded",
  "stage": "done",
  "error_code": "ok",
  "output_path": "/abs/output/dQw4w9WgXcQ.mp3",
  "download_ms": 1200,
  "convert_ms": 400,
  "bytes": 1048576,
  "source_bytes": 1048576,
  "output_bytes": 4194304,
  "attached": false,
  "shared_download": false,
  "source_cache_hit": false,
  "reused": false
}
```

### Synchronous mode

With `YTCONV_SYNC_CONVERSIONS=1`, `POST /v1/conversions` answers when the job finishes instead of
returning 202. Synchronous requests go through the same queue as asynchronous ones: the same
operation and child limits, the same 503 `busy` admission rejection (returned immediately), the
same sharing with equivalent requests, and the same cancellation (`DELETE` still works). The
response is held open without blocking a listener thread. Success returns 200 with the job
status fields and `"status": "success"`; failures return the typed status (for example 500
`download_failed`, 503 `binary_not_found`, 504 `timeout`); a response still open at shutdown gets
503. A client disconnect does not cancel the job, because other clients may share its operation;
use `DELETE /v1/jobs/{id}` to cancel.

### Jobs, shared operations, and history

Each accepted request gets its own job ID. Equivalent concurrent requests (same video, format,
and reuse policy) subscribe to one shared *operation* that performs the download/encode; their
job status shows `"attached": true`. MP3 and WAV jobs for the same video also share one download.

Job history is bounded and process-local (lost on restart):

- A terminal job (succeeded, failed, canceled) is retained for `YTCONV_JOB_HISTORY_TTL_SEC`
  (default 3600) after it finished, and at most `YTCONV_JOB_HISTORY_MAX` (default 1024) terminal
  jobs are kept; the oldest are dropped first. An expired or dropped job ID returns 404.
- Live (queued or running) jobs are never dropped. At most `YTCONV_MAX_ACTIVE_JOBS` (default 256)
  live jobs exist, including attached ones; beyond that `POST` returns 503 `busy`.
- `YTCONV_QUEUE_DEPTH` (default 8) bounds queued plus running *operations*, not jobs; a new
  operation beyond it returns 503 `busy`. `YTCONV_MAX_CONCURRENT` operations execute at once.

These defaults are an initial policy sized for a personal service, not a benchmark result.

### Cancel job

`DELETE /v1/jobs/{job_id}` cancels that one job. It detaches the job from its operation; other
jobs sharing the operation (or its download) keep running and still get their result. The
operation, and its child process, is canceled only when its last interested job is canceled
(SIGTERM, then SIGKILL after two seconds). A canceled job stays canceled even if the shared
work later succeeds. A second `DELETE` of a finished job returns 409.

```json
{
  "job_id": "dQw4w9WgXcQ-mp3",
  "status": "canceled"
}
```

### Errors

```json
{
  "status": "error",
  "error_code": "invalid_url",
  "message": "URL must be a YouTube video link"
}
```

| Status | Typical `error_code` |
|---|---|
| 400 | `invalid_url`, `unsupported_host`, `unsupported_format`, `playlist_only`, `channel_url`, `invalid_input` |
| 401 | `unauthorized` |
| 404 | unknown path, job not found |
| 405 | GET `/v1/conversions` |
| 409 | cancel of finished job |
| 500 | `download_failed`, `conversion_failed`, `internal_error` |
| 503 | `busy`, `binary_not_found`, shutdown |
| 504 | `timeout` |
| 507 | `disk_full` |

`GET /v1/conversions` returns 405 and `Allow: POST`.

## Request limits

Request intake is bounded before any application code runs:

- Decoded request bodies are limited to 8,192 bytes, with or without `Content-Length`. A valid
  `Content-Length` above the limit is rejected with 400 before the body is read; a chunked body
  is rejected with 400 as soon as its decoded size would exceed the limit. Invalid or
  conflicting framing (non-numeric or duplicate `Content-Length`, `Content-Length` with
  `Transfer-Encoding`, encodings other than `chunked`, malformed chunks) returns 400. Request
  headers over 16 KiB return 431.
- The whole request (headers and body) must arrive within `YTCONV_REQUEST_READ_TIMEOUT_SEC`
  (default 10, separate from conversion timeouts); otherwise 408 and the connection is closed.
- At most `YTCONV_MAX_PENDING_READS` (default 32) connections may still be sending a request and
  at most `YTCONV_MAX_CONNECTIONS` (default 128) may be open, including ones waiting for a
  synchronous response; beyond that new connections get 503.
- Each connection carries one request (`Connection: close`). Oversized input is not drained to
  keep the connection alive; the server answers, discards at most 64 KiB for 0.5 s so the client
  can read the answer, and closes.

Why a gate: cpprestsdk's asio listener (checked in 2.10.18, the version Debian ships) reads every
request body, `Content-Length` or chunked, into an in-memory buffer independently of the handler
and has no read timeout, so a size check in the handler cannot bound memory. The API therefore
owns the public socket with a small in-process gate that enforces the limits above and forwards
only validated, `Content-Length`-framed requests to cpprest on a private loopback port. Requests
reaching that port without the gate's per-process secret are rejected with 403.

## Probes

`GET /v1/healthz` → `{"status":"ok"}`

`GET /v1/readyz` → 200 `{"status":"ready"}` if `yt-dlp` and `ffmpeg` respond and the output directory is writable; otherwise 503. Tool checks are cached for 60 seconds.

`GET /v1/metrics` (loopback only) returns in-process counters (reset on restart).

## Metrics

Physical work is counted once where it happens, however many jobs share it. Logical job
outcomes are counted once per client job. Gauges are absolute values read from queue state.

| Field | Kind | Meaning |
|---|---|---|
| `jobs_started` | logical | Accepted jobs: queued, attached, or answered by immediate reuse |
| `jobs_succeeded` / `jobs_failed` / `jobs_canceled` | logical | Terminal outcome of each job, exactly once |
| `jobs_reused` | logical | Successful jobs satisfied by a completed output file (subset of succeeded) |
| `jobs_coalesced` | logical | Jobs attached to an existing operation |
| `jobs_queued` / `jobs_running` / `jobs_active` | gauge | Live jobs by operation state; `jobs_active` is their sum |
| `operations_queued` / `operations_running` | gauge | Executable operations (each runs at most one download and one encode) |
| `downloads_total` / `download_failures_total` | physical | yt-dlp runs that produced a source / failed or were canceled |
| `download_ms_total` | physical | Duration of every download attempt, counted once |
| `source_bytes_downloaded` | physical | Size of each newly downloaded source file. An approximation of media bytes, not wire traffic (no retries, metadata, headers, or merge overhead). Failed downloads add 0. `bytes_downloaded` is a deprecated alias with the same value |
| `source_cache_hits_total` / `shared_downloads_total` | physical | Operations served by a cached source / by joining another operation's download (both add 0 downloaded bytes) |
| `encodes_total` / `encode_failures_total` / `convert_ms_total` | physical | ffmpeg runs and their total duration |
| `outputs_published_total` / `bytes_written` | physical | Outputs renamed into place and their sizes; reuse writes nothing |
| `queue_ms_total` | logical | Time jobs waited before their operation started |
| `ready_checks` / `ready_spawns` | probe | Readiness checks and those that spawned tool probes |

Job status fields `download_ms` and `convert_ms` describe the operation the job used (a job that
shared a download shows that download's duration); they never add to the totals again.
`source_bytes` is the size of the source file used, `output_bytes` the size of the output,
`bytes` a compatibility alias of `source_bytes`, and `shared_download`/`source_cache_hit` say how
the source was obtained.

Changes in meaning from earlier builds: `bytes_downloaded` used to add the source size for every
successful job, including reuse and shared downloads, and `bytes_written` included reused
outputs; both now count new physical work only. `jobs_active` is now a gauge of live jobs.

All responses include `Cache-Control: no-store`. There is no `Access-Control-Allow-Origin: *`.

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `YTCONV_MAX_CONCURRENT` | `2` | Operations executing at once (worker threads) |
| `YTCONV_QUEUE_DEPTH` | `8` | Queued plus running operations |
| `YTCONV_MAX_ACTIVE_JOBS` | `256` | Live client jobs, including attached ones (must be ≥ queue depth) |
| `YTCONV_JOB_HISTORY_MAX` | `1024` | Retained terminal job snapshots |
| `YTCONV_JOB_HISTORY_TTL_SEC` | `3600` | Lifetime of a terminal job snapshot after it finished |
| `YTCONV_DOWNLOAD_TIMEOUT_SEC` | `600` | yt-dlp timeout |
| `YTCONV_CONVERT_TIMEOUT_SEC` | `300` | ffmpeg timeout |
| `YTCONV_CONCURRENT_FRAGMENTS` | `4` | yt-dlp fragment concurrency |
| `YTCONV_FRAGMENT_RETRIES` | `10` | yt-dlp fragment retries |
| `YTCONV_READY_TTL_SEC` | `60` | Readiness probe cache TTL |
| `YTCONV_CACHE_DIR` | `<output>/cache/ytdlp` | yt-dlp metadata cache. A relative path is under the output root |
| `YTCONV_SOURCE_CACHE_TTL_SEC` | `86400` | Reuse a downloaded source for this many seconds |
| `YTCONV_SOURCE_CACHE_MAX_BYTES` | `10G` | Evict the oldest cached sources above this size |
| `YTCONV_REQUEST_READ_TIMEOUT_SEC` | `10` | Time allowed to receive a whole request |
| `YTCONV_MAX_PENDING_READS` | `32` | Connections still sending their request |
| `YTCONV_MAX_CONNECTIONS` | `128` | Open client connections (must be ≥ pending reads) |
| `YTCONV_REUSE_COMPLETED` | `1` | `0` disables completed-output reuse (every request re-encodes) |
| `YTCONV_SYNC_CONVERSIONS` | unset | `1` keeps the `POST /v1/conversions` response open until the job finishes |
