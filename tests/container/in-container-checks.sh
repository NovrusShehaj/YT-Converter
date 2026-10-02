#!/bin/sh
# Runs inside the runtime image as its configured user. Needs the repository's tests/fakes
# mounted at /fakes and a writable /data/output. Uses no network and no YouTube URL fetches:
# yt-dlp and ffmpeg are the fakes.
set -eu

fail() {
  echo "FAIL: $*" >&2
  if [ -f /data/output/api.log ]; then
    tail -n 20 /data/output/api.log >&2
  fi
  exit 1
}

uid=$(id -u)
[ "$uid" = "10001" ] || fail "expected UID 10001, got $uid"
touch /data/output/.write-probe && rm /data/output/.write-probe || fail "/data/output is not writable by UID $uid"

yt2mp3-cli --help >/dev/null || fail "yt2mp3-cli --help"
yt2mp3-cli --version || fail "yt2mp3-cli --version"
yt2mp3-api --version || fail "yt2mp3-api --version"
sh /usr/local/lib/ytconv/verify-runtime.sh /usr/local/bin/yt2mp3-cli /usr/local/bin/yt2mp3-api \
  >/dev/null || fail "runtime dependency check"

key="container-smoke-key"
port="${SMOKE_PORT:-18080}"
gate=/data/output/gate
mkdir -p "$gate" /data/output/fake-logs
touch "$gate/release" # the first download is not held

YTCONV_YT_DLP=/fakes/yt-dlp YTCONV_FFMPEG=/fakes/ffmpeg \
YTCONV_FAKE_LOG_DIR=/data/output/fake-logs YTCONV_YTDLP_GATE_DIR="$gate" \
YTCONV_API_KEY="$key" YTCONV_ALLOW_UNAUTHENTICATED_LOCALHOST= YTCONV_PORT="$port" \
  yt2mp3-api > /data/output/api.log 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT
base="http://127.0.0.1:$port"

for _ in $(seq 1 100); do
  curl -fsS "$base/v1/healthz" >/dev/null 2>&1 && break
  sleep 0.1
done
curl -fsS "$base/v1/healthz" | grep -q '"ok"' || fail "healthz"
curl -fsS "$base/v1/readyz" | grep -q '"ready"' || fail "readyz"

post() { # format video [key]
  curl -sS -o /tmp/post.json -w '%{http_code}' -X POST "$base/v1/conversions" \
    -H 'Content-Type: application/json' ${3:+-H "X-Api-Key: $3"} \
    -d "{\"url\":\"https://www.youtube.com/watch?v=$2\",\"format\":\"$1\"}"
}
field() { python3 -c "import json,sys; print(json.load(open('$1')).get('$2',''))"; }

code=$(post mp3 dQw4w9WgXcQ)
[ "$code" = "401" ] || fail "conversion without key returned $code"

code=$(post mp3 dQw4w9WgXcQ "$key")
[ "$code" = "202" ] || fail "conversion returned $code"
job=$(field /tmp/post.json job_id)
status=""
for _ in $(seq 1 100); do
  curl -sS -H "X-Api-Key: $key" "$base/v1/jobs/$job" -o /tmp/job.json
  status=$(field /tmp/job.json status)
  [ "$status" = "succeeded" ] || [ "$status" = "failed" ] && break
  sleep 0.1
done
[ "$status" = "succeeded" ] || fail "job status $status"
[ -s /data/output/dQw4w9WgXcQ.mp3 ] || fail "output missing or empty"

# A held download: cancellation requires the key and leaves the job canceled.
rm -f "$gate/release"
code=$(post wav jNQXAC9IVRw "$key")
[ "$code" = "202" ] || fail "second conversion returned $code"
job=$(field /tmp/post.json job_id)
code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$base/v1/jobs/$job")
[ "$code" = "401" ] || fail "cancel without key returned $code"
code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE -H "X-Api-Key: $key" "$base/v1/jobs/$job")
[ "$code" = "200" ] || fail "authorized cancel returned $code"
curl -sS -H "X-Api-Key: $key" "$base/v1/jobs/$job" -o /tmp/job.json
[ "$(field /tmp/job.json status)" = "canceled" ] || fail "job was not canceled"
touch "$gate/release"

kill -TERM "$pid"
for _ in $(seq 1 100); do
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.1
done
kill -0 "$pid" 2>/dev/null && fail "API did not stop after SIGTERM"
trap - EXIT
echo "in-container checks passed"
