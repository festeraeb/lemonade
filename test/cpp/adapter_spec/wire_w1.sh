#!/usr/bin/env bash
# W1: AdapterServer::load() reads the spec through check_spec, build_argv and build_env.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
f="$root/src/cpp/server/backends/adapter/adapter_server.cpp"
fail=0
for fn in check_spec build_argv build_env; do
    grep -q "specs::${fn}(" "$f" || { echo "FAIL: load() does not call specs::${fn}"; fail=1; }
done
grep -q 'spec\["args"\]' "$f" && { echo "FAIL: load() still reads spec[\"args\"] itself; use build_argv"; fail=1; }
grep -q 'env.items()' "$f" && { echo "FAIL: load() still loops env.items() itself; use build_env"; fail=1; }
grep -q 'SpecError' "$f" || { echo "FAIL: load() does not turn specs::SpecError into an error naming the adapter"; fail=1; }
[ "$fail" = 0 ] || exit 1
echo "w1 wiring: calls present"
bash "$root/test/cpp/adapter_spec/wire_check.sh"
