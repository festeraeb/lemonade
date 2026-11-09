#!/usr/bin/env bash
# W1b: an engine whose binary sits in <engine>/bin/ may use library paths under <engine>/.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
f="$root/src/cpp/server/backends/adapter/adapter_server.cpp"
grep -A12 'allowed_dirs' "$f" | grep -q '"bin"' || { echo "FAIL: allowed_dirs does not add the engine root when the binary is in a bin/ folder"; exit 1; }
echo "w1b: engine-root rule present"
bash "$root/test/cpp/adapter_spec/wire_w1.sh"
