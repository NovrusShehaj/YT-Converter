#!/usr/bin/env bash
# Exercises the documented docker-compose.yml (host network, ./output bind mount). Run from the
# repository root on a host where port 8080 is free. Requires sudo for the ownership cases.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
trap 'docker compose down --remove-orphans >/dev/null 2>&1 || true' EXIT

wait_health() {
  for _ in $(seq 1 60); do
    curl -fsS http://127.0.0.1:8080/v1/healthz >/dev/null 2>&1 && return 0
    sleep 1
  done
  docker compose logs >&2
  return 1
}

# Documented setup: the bind-mounted output directory is owned by the container user.
sudo rm -rf output
mkdir -p output
sudo chown 10001:10001 output
docker compose up -d --build
wait_health
curl -fsS http://127.0.0.1:8080/v1/readyz | grep -q '"ready"'
docker compose down

# A root-owned bind mount (what Docker creates when ./output is missing) is a deployment error,
# not a code failure: the service stays healthy and readiness names the cause.
sudo rm -rf output
mkdir -p output
sudo chown 0:0 output
sudo chmod 0755 output
docker compose up -d
wait_health
code="$(curl -sS -o /tmp/ytconv-ready.json -w '%{http_code}' http://127.0.0.1:8080/v1/readyz)"
[ "$code" = "503" ]
grep -q 'not writable' /tmp/ytconv-ready.json
docker compose down
sudo rm -rf output
echo "compose smoke test passed"
