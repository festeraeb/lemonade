#!/usr/bin/env bash
# W2 (R11): the card is chosen through specs::choose_device from lemond's own GPU list.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
f="$root/src/cpp/server/backends/adapter/adapter_server.cpp"
fail=0
grep -q 'specs::choose_device(' "$f" || { echo "FAIL: load() does not call specs::choose_device"; fail=1; }
grep -q '"{gpu_uuids}"' "$f" || { echo "FAIL: load() does not supply the {gpu_uuids} placeholder"; fail=1; }
grep -q 'get_nvidia_gpu_devices' "$f" || { echo "FAIL: load() does not read lemond's NVIDIA GPU list"; fail=1; }
[ "$fail" = 0 ] || exit 1
echo "w2: device choice wired"
bash "$root/test/cpp/adapter_spec/wire_check.sh"
