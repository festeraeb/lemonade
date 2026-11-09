#!/usr/bin/env bash
# W2b: load() takes {device} and {gpu_uuids} from specs::device_values instead of its own copy of the rule.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
f="$root/src/cpp/server/backends/adapter/adapter_server.cpp"
fail=0
grep -q 'specs::device_values(' "$f" || { echo "FAIL: load() does not call specs::device_values"; fail=1; }
grep -q 'specs::choose_device(' "$f" && { echo "FAIL: load() still calls specs::choose_device itself (device_values does that)"; fail=1; }
grep -q '"{gpu_uuids}"' "$f" || { echo "FAIL: load() no longer supplies {gpu_uuids}"; fail=1; }
[ "$fail" = 0 ] || exit 1
echo "w2b: device_values used"
bash "$root/test/cpp/adapter_spec/wire_check.sh"
