#!/usr/bin/env python3
"""Live check of the spec's prepare step against a built lemond.

    python3 test/cpp/adapter_spec/live/prepare_live.py <path to lemond>

Starts lemond on 127.0.0.1:$ADAPTER_LIVE_PORT (default 13813). The prepare program is the fixture
fake_prepare.py and the engine is the fixture fake_engine.py; neither is a real engine. Checks that the
step runs before the first load with its output in the log, is not run again while its inputs are
unchanged, runs again when an input changes, fails the load with its last lines (without the router's
evict-everything retry), honours its timeout, cannot use {port}, and passes text to the program without
a shell."""

import json, os, shutil, sys, time
from harness import Live, HERE

live = Live(sys.argv[1], 13813)
T = live.t
shutil.copy(HERE + "/fake_prepare.py", T + "/adapters/fake_prepare.py")
os.chmod(T + "/adapters/fake_prepare.py", 0o755)
for m in ("good", "fail", "slow", "port", "other"):
    open(f"{T}/models/{m}.gguf", "w").write("x")


def spec(name, prepare_args, timeout_s=None):
    prep = {"args": ["{spec_dir}/fake_prepare.py"] + prepare_args}
    if timeout_s:
        prep["timeout_s"] = timeout_s
    live.spec(
        name,
        {
            "spec_version": 1,
            "binary": T + "/fake_engine.py",
            "args": ["--port", "{port}"],
            "env": {"LEMONADE_ENGINE_RECORD": f"{T}/{name}.rec"},
            "ready": {"path": "/health", "timeout_s": 30},
            "prepare": prep,
            "models": [{"name": name, "path": f"{T}/models/{name[2:]}.gguf"}],
        },
    )


spec("p-good", ["--model", "{model_path}", "--count", T + "/count", "$(touch " + T + "/pwned)"])
spec("p-fail", ["--model", "{model_path}", "--fail"])
spec("p-slow", ["--model", "{model_path}", "--sleep", "30"], timeout_s=2)
spec("p-port", ["--model", "{model_path}", "--port", "{port}"])
spec("p-noprog", ["--model", "{model_path}"])
live.spec("p-noprog", dict(json.load(open(T + "/adapters/p-noprog.json")), prepare={"args": ["no-such-prepare-tool"]}))
open(T + "/models/noprog.gguf", "w").write("x")
live.spec(
    "p-other",
    {
        "spec_version": 1,
        "binary": T + "/fake_engine.py",
        "args": ["--port", "{port}"],
        "env": {"LEMONADE_ENGINE_RECORD": f"{T}/p-other.rec"},
        "ready": {"path": "/health", "timeout_s": 30},
        "models": [{"name": "p-other", "path": T + "/models/other.gguf"}],
    },
)


def runs():
    return len(open(T + "/count").read().splitlines()) if os.path.exists(T + "/count") else 0


try:
    live.start()
    st, body = live.call("/api/v1/load", {"model_name": "p-good"})
    log = live.log_text()
    live.check(
        "prepare runs before the first load, output in the log",
        st == 200 and runs() == 1 and os.path.exists(T + "/models/good.gguf.prepared")
        and "p-good prepare: fixture prepare: converting" in log and "p-good: prepare done" in log,
        f"status {st}, runs {runs()}",
    )
    live.check(
        "text reaches the program as one argument, never a shell",
        not os.path.exists(T + "/pwned") and "$(touch" in log,
        "no file created by $(touch ...)" if not os.path.exists(T + "/pwned") else "a shell ran it",
    )
    live.call("/api/v1/unload", {"model_name": "p-good"})
    st, body = live.call("/api/v1/load", {"model_name": "p-good"})
    live.check(
        "not run again while its inputs are unchanged",
        st == 200 and runs() == 1 and "prepare already done for these inputs" in live.log_text(),
        f"status {st}, runs {runs()}",
    )
    live.call("/api/v1/unload", {"model_name": "p-good"})
    time.sleep(1.1)
    open(T + "/models/good.gguf", "w").write("changed")
    st, body = live.call("/api/v1/load", {"model_name": "p-good"})
    live.check("run again when an input changes", st == 200 and runs() == 2, f"status {st}, runs {runs()}")

    st, body = live.call("/api/v1/load", {"model_name": "p-other"})
    loaded_before = st == 200
    st, body = live.call("/api/v1/load", {"model_name": "p-fail"})
    log = live.log_text()
    live.check(
        "a failing prepare fails the load with its last lines",
        st >= 400 and "prepare failed (exit 3)" in body and "fixture prepare failed on purpose" in body
        and "unsupported tensor type" in body,
        f"status {st}: {body[:300]}",
    )
    live.check(
        "and does not trigger the evict-everything retry",
        "evicting all models and retrying" not in log and log.count("p-fail: prepare: ") == 1,
        f"prepare started {log.count('p-fail: prepare: ')} time(s)",
    )
    t0 = time.time()
    st, body = live.call("/api/v1/load", {"model_name": "p-slow"})
    took = time.time() - t0
    live.check(
        "the timeout stops a slow prepare",
        st >= 400 and "timed out after 2 s" in body and took < 15,
        f"status {st} after {took:.1f}s: {body[:200]}",
    )
    st, body = live.call("/api/v1/load", {"model_name": "p-port"})
    live.check("prepare cannot use {port}", st >= 400 and "{port}" in body, f"status {st}: {body[:200]}")
    st, body = live.call("/api/v1/load", {"model_name": "p-noprog"})
    live.check(
        "a missing prepare program is named, with no evict-everything retry",
        st >= 400 and "prepare.args[0]" in body and "evicting all models and retrying" not in live.log_text(),
        f"status {st}: {body[:200]}",
    )
finally:
    sys.exit(live.finish("prepare live"))
