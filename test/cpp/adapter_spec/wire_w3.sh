#!/usr/bin/env bash
# W3 (R9): the adapter backend serves the engine's memory report in its telemetry.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
f="$root/src/cpp/server/backends/adapter/adapter_server.cpp"
h="$root/src/cpp/include/lemon/backends/adapter/adapter_server.h"
fail=0
grep -q 'memory_path_' "$h" || { echo "FAIL: AdapterServer has no memory_path_ member"; fail=1; }
grep -q "specs::memory_telemetry(" "$f" || { echo "FAIL: telemetry does not use specs::memory_telemetry"; fail=1; }
grep -q '"adapter_memory"' "$f" || { echo "FAIL: telemetry has no adapter_memory entry"; fail=1; }
[ "$fail" = 0 ] || exit 1
echo "w3: memory report wired"
bash "$root/test/cpp/adapter_spec/wire_check.sh"
