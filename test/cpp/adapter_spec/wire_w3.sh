#!/usr/bin/env bash
# W3 (R9): the adapter backend serves the engine's memory report in its telemetry.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
f="$root/src/cpp/server/backends/adapter/adapter_server.cpp"
h="$root/src/cpp/include/lemon/backends/adapter/adapter_server.h"
fail=0
grep -q 'memory_path_' "$h" || { echo "FAIL: AdapterServer has no memory_path_ member"; fail=1; }
grep -q 'specs::parse_memory_report(' "$f" || { echo "FAIL: telemetry does not use specs::parse_memory_report"; fail=1; }
grep -q 'specs::local_bytes(' "$f" || { echo "FAIL: telemetry does not report specs::local_bytes"; fail=1; }
grep -q '"adapter_memory"' "$f" || { echo "FAIL: telemetry has no adapter_memory entry"; fail=1; }
[ "$fail" = 0 ] || exit 1
echo "w3: memory report wired"
bash "$root/test/cpp/adapter_spec/wire_check.sh"
