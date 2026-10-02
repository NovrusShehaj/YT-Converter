#!/usr/bin/env bash
# Container gate: verifies the runtime image as built, as its configured user, without live
# YouTube traffic. Usage: tests/container/smoke.sh IMAGE
set -euo pipefail
image="${1:?usage: $0 IMAGE}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
volume="ytconv-smoke-$$"
health="ytconv-health-$$"

cleanup() {
  docker rm -f "$health" >/dev/null 2>&1 || true
  docker volume rm -f "$volume" >/dev/null 2>&1 || true
}
trap cleanup EXIT

user="$(docker image inspect -f '{{.Config.User}}' "$image")"
[ "$user" = "ytconv" ] || { echo "image user is '$user', expected ytconv" >&2; exit 1; }

# A named volume is initialized from the image's /data/output, so it is owned by UID 10001.
# (A host bind mount must be chowned to 10001 first; see README "Docker".)
docker volume create "$volume" >/dev/null
docker run --rm -v "$volume:/data/output" \
  -v "$root/tests/fakes:/fakes:ro" -v "$root/tests/container:/checks:ro" \
  --entrypoint /bin/sh "$image" /checks/in-container-checks.sh

# The image as configured: default entrypoint, loopback bind, and its HEALTHCHECK.
docker run -d --name "$health" --health-interval=1s --health-start-period=1s "$image" >/dev/null
for _ in $(seq 1 60); do
  state="$(docker inspect -f '{{.State.Health.Status}}' "$health")"
  [ "$state" = "healthy" ] && break
  sleep 1
done
[ "$state" = "healthy" ] || { docker logs "$health" >&2; echo "healthcheck: $state" >&2; exit 1; }
[ "$(docker exec "$health" id -u)" = "10001" ] || { echo "service is not UID 10001" >&2; exit 1; }
docker exec "$health" curl -fsS http://127.0.0.1:8080/v1/readyz
echo
echo "container smoke test passed"
