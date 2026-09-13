#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if grep -R -n -E 'system[[:space:]]*\(|popen[[:space:]]*\(' "$ROOT/src"; then
  echo "Forbidden shell process API found in src/" >&2
  exit 1
fi
echo "No system() or popen() in src/"
