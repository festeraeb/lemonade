#!/usr/bin/env python3
"""Live check of the spec's health probe against a built lemond.

    python3 test/cpp/adapter_spec/live/health_probe_live.py <path to lemond>

Starts lemond on 127.0.0.1:$ADAPTER_LIVE_PORT (default 13814) and loads the fixture engine
(fake_engine.py, a test stand-in). POST /fixture/hang on the fixture makes its /health and chat stop
answering while the process stays alive, as a hung engine would. Checks that a spec with "health" is
marked unhealthy after fail_after missed probes and its process stopped, that the next request reloads
a fresh engine, and that a spec without "health" behaves as before (an idle hung engine is not noticed)."""

import json, os, socket, sys, time, urllib.request
from harness import Live

live = Live(sys.argv[1], 13814)
T = live.t
open(T + "/models/m.gguf", "w").write("x")


def spec(name, health=None):
    s = {
        "spec_version": 1,
        "binary": T + "/fake_engine.py",
        "args": ["--port", "{port}"],
        "env": {"LEMONADE_ENGINE_RECORD": f"{T}/{name}.rec"},
        "ready": {"path": "/health", "timeout_s": 30},
        "models": [{"name": name, "path": T + "/models/m.gguf"}],
    }
    if health is not None:
        s["health"] = health
    live.spec(name, s)


spec("h-probe", {"path": "/health", "interval_s": 1, "timeout_ms": 500, "fail_after": 2})
spec("h-none")
spec("h-bad", {"path": "/health", "timeout_ms": "fast"})


def engine_port(name):
    argv = json.load(open(f"{T}/{name}.rec"))["argv"]
    return int(argv[argv.index("--port") + 1])


def hang(port):
    req = urllib.request.Request(f"http://127.0.0.1:{port}/fixture/hang", data=b"{}", method="POST")
    urllib.request.urlopen(req, timeout=5).read()


def listening(port):
    with socket.socket() as s:
        s.settimeout(1)
        return s.connect_ex(("127.0.0.1", port)) == 0


def models_in_health():
    st, h = live.call("/api/v1/health")
    return {m.get("model_name"): m for m in json.loads(h).get("all_models_loaded", [])}


def chat(model, timeout=60):
    return live.call(
        "/api/v1/chat/completions",
        {"model": model, "messages": [{"role": "user", "content": "hi"}]},
        timeout=timeout,
    )


try:
    live.start()
    st, body = live.call("/api/v1/load", {"model_name": "h-probe"})
    st2, answer = chat("h-probe")
    live.check(
        "a spec with a health probe loads and answers",
        st == 200 and st2 == 200 and "fake engine answer" in answer,
        f"load {st}, chat {st2}",
    )
    port = engine_port("h-probe")
    t0 = time.time()
    hang(port)
    gone = None
    while time.time() - t0 < 20:
        if "h-probe" not in models_in_health() or not listening(port):
            gone = time.time() - t0
            break
        time.sleep(0.25)
    log = live.log_text()
    live.check(
        "a hung engine is marked unhealthy after fail_after missed probes",
        gone is not None and "health probe /health missed 2/2" in log
        and "adapter health probe: /health missed 2 times in a row" in log,
        f"noticed after {gone if gone is None else round(gone, 1)} s",
    )
    live.check("its process is stopped", not listening(port), f"engine port {port} listening={listening(port)}")
    st, answer = chat("h-probe")
    new_port = engine_port("h-probe")
    live.check(
        "the next request reloads a fresh engine and is answered",
        st == 200 and "fake engine answer" in answer and new_port != port,
        f"chat {st}, engine port {port} -> {new_port}",
    )
    live.call("/api/v1/unload", {"model_name": "h-probe"})

    st, body = live.call("/api/v1/load", {"model_name": "h-none"})
    port = engine_port("h-none")
    hang(port)
    time.sleep(6)
    live.check(
        "without health, an idle hung engine is not noticed (unchanged)",
        "h-none" in models_in_health() and listening(port),
        f"loaded {list(models_in_health())}",
    )
    live.call("/api/v1/unload", {"model_name": "h-none"}, timeout=120)

    st, body = live.call("/api/v1/load", {"model_name": "h-bad"})
    live.check(
        "a malformed health object is refused naming the field",
        st >= 400 and "health.timeout_ms" in body,
        f"status {st}: {body[:160]}",
    )
finally:
    sys.exit(live.finish("health probe live"))
