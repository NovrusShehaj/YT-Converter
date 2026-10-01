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

## Convert

`POST /v1/conversions`

```json
{"url":"https://www.youtube.com/watch?v=dQw4w9WgXcQ","format":"mp3"}
```

Optional header: `X-Api-Key` when an API key is configured.

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

When the output file already exists and `reuse_completed` is enabled, the API returns 200 with `reused: true`. Otherwise it returns 202 `queued` and the conversion runs asynchronously.

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
  "reused": false
}
```

### Cancel job

`DELETE /v1/jobs/{job_id}` cancels a running job.

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

## Probes

`GET /v1/healthz` → `{"status":"ok"}`

`GET /v1/readyz` → 200 `{"status":"ready"}` if `yt-dlp` and `ffmpeg` respond and the output directory is writable; otherwise 503. Tool checks are cached for 60 seconds.

`GET /v1/metrics` (loopback only) returns in-process counters including `download_ms_total`, `convert_ms_total`, `queue_ms_total`, and `bytes_downloaded`.

All responses include `Cache-Control: no-store`. There is no `Access-Control-Allow-Origin: *`.

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `YTCONV_MAX_CONCURRENT` | `2` | Worker threads |
| `YTCONV_QUEUE_DEPTH` | `8` | Max queued jobs |
| `YTCONV_DOWNLOAD_TIMEOUT_SEC` | `600` | yt-dlp timeout |
| `YTCONV_CONVERT_TIMEOUT_SEC` | `300` | ffmpeg timeout |
| `YTCONV_CONCURRENT_FRAGMENTS` | `4` | yt-dlp fragment concurrency |
| `YTCONV_FRAGMENT_RETRIES` | `10` | yt-dlp fragment retries |
| `YTCONV_READY_TTL_SEC` | `60` | Readiness probe cache TTL |
| `YTCONV_CACHE_DIR` | `<output>/cache/ytdlp` | yt-dlp metadata cache. A relative path is under the output root |
| `YTCONV_SOURCE_CACHE_TTL_SEC` | `86400` | Reuse a downloaded source for this many seconds |
| `YTCONV_SOURCE_CACHE_MAX_BYTES` | `10G` | Evict the oldest cached sources above this size |
| `YTCONV_SYNC_CONVERSIONS` | unset | `1` makes `POST /v1/conversions` block until the file is ready |
