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

### 200

```json
{
  "status": "success",
  "error_code": "ok",
  "message": "Conversion completed",
  "output_path": "/absolute/path/output/dQw4w9WgXcQ.mp3",
  "job_id": "dQw4w9WgXcQ-1a2b3c4d"
}
```

`output_path` is an absolute filesystem path on the machine running the API.

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
| 404 | unknown path |
| 405 | GET `/v1/conversions` |
| 500 | `download_failed`, `conversion_failed`, `internal_error` |
| 503 | `busy`, `binary_not_found`, shutdown |
| 504 | `timeout` |
| 507 | `disk_full` |

`GET /v1/conversions` returns 405 and `Allow: POST`.

## Probes

`GET /v1/healthz` → `{"status":"ok"}`

`GET /v1/readyz` → 200 `{"status":"ready"}` if `yt-dlp` and `ffmpeg` respond and the output directory is writable; otherwise 503.

`GET /v1/metrics` (loopback only) returns in-process counters.

All responses include `Cache-Control: no-store`. There is no `Access-Control-Allow-Origin: *`.
