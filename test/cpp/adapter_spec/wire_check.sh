#!/usr/bin/env bash
# Checks wiring changes in adapter_server.cpp without building lemond: replays that file's compile
# command from an existing configured build (syntax only), then runs every adapter_spec job test.
#   bash test/cpp/adapter_spec/wire_check.sh
# BUILD_DIR overrides the configured build whose compile_commands.json is replayed.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
build="${BUILD_DIR:-/data/builds/adapter-plane/build}"
cmd="$(python3 - "$build/compile_commands.json" "$root" <<'PY'
import json, shlex, sys
db, root = sys.argv[1], sys.argv[2]
e = [x for x in json.load(open(db)) if x["file"].endswith("backends/adapter/adapter_server.cpp")][0]
args = e.get("arguments") or shlex.split(e["command"])
src_root = e["file"].rsplit("/src/cpp/", 1)[0]
out, skip = [], False
for a in args:
    if skip:
        skip = False
        continue
    if a in ("-o", "-MF", "-MT", "-MQ"):
        skip = True
        continue
    if a in ("-c", "-MD", "-MMD"):
        continue
    out.append(a.replace(src_root, root))
out.append("-fsyntax-only")
print(shlex.join(out))
PY
)"
echo "compiling adapter_server.cpp (syntax only)"
eval "$cmd"
for t in "$root"/test/cpp/adapter_spec/test_ap*.cpp; do
    job="$(basename "$t" .cpp)"; job="${job#test_}"
    bash "$root/test/cpp/adapter_spec/run.sh" "$job"
done
