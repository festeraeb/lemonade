#!/usr/bin/env python3
"""Live check of the budget request the eviction engine makes before unloading an adapter model.

    python3 test/cpp/adapter_spec/live/budget_live.py <path to lemond>

Starts lemond on 127.0.0.1:$ADAPTER_LIVE_PORT (default 13812) with auto_evict on, loads the fixture
engine (fake_engine.py, a test stand-in whose memory numbers are made up here) under specs that differ
only in how its budget route answers, and drives pressure eviction through
/internal/simulate-vram-pressure. No GPU is used: lemond sees no GPU tool and the fixture holds nothing.
With no device size to read, the budget is the engine's own bytes scaled by threshold / pressure."""

import json, os, sys, threading, time
from harness import Live

live = Live(sys.argv[1], 13812)
T = live.t
open(T + "/models/m.gguf", "w").write("x")
with open(T + "/home/.config/lemonade/config.json", "w") as f:
    json.dump({"auto_evict": True, "auto_evict_threshold_pct": 0.9}, f)


def spec(name, budget, budget_path="/budget"):
    s = {
        "spec_version": 1,
        "binary": T + "/fake_engine.py",
        "args": ["--port", "{port}"],
        "env": {
            "LEMONADE_ENGINE_RECORD": f"{T}/{name}.rec",
            "LEMONADE_ENGINE_MEM_MIB": "1024",
            "LEMONADE_ENGINE_UUID": "{gpu_uuids}",
            "LEMONADE_ENGINE_BUDGET": budget,
        },
        "default_device_index": "0",
        "ready": {"path": "/health", "timeout_s": 30},
        "memory_path": "/memory",
        "models": [{"name": name, "path": T + "/models/m.gguf"}],
    }
    if budget_path:
        s["budget_path"] = budget_path
    live.spec(name, s)


for mode in ("accept", "refuse", "short", "lie", "garbage", "slow"):
    spec("m-" + mode, mode)
spec("m-plain", "accept", budget_path=None)
spec("m-idle", "accept")


def loaded():
    st, h = live.call("/api/v1/health")
    return [m.get("model_name") for m in json.loads(h).get("all_models_loaded", [])]


def budget_requests(name):
    path = f"{T}/{name}.rec.budget"
    return [json.loads(l) for l in open(path)] if os.path.exists(path) else []


def pressure(pct):
    t0 = time.time()
    st, body = live.call("/internal/simulate-vram-pressure", {"pct": pct}, timeout=90)
    return st, time.time() - t0


try:
    live.start()

    st, body = live.call("/api/v1/load", {"model_name": "m-accept"})
    st, took = pressure(0.95)
    reqs = budget_requests("m-accept")
    live.check(
        "the engine is asked to shrink before it is unloaded",
        len(reqs) == 1 and reqs[0].get("mib") == 970,
        f"budget requests {reqs} (1024 MiB * 0.90 / 0.95 = 970)",
    )
    live.check(
        "an engine that shrinks stays loaded",
        "m-accept" in loaded() and "shrank instead of being unloaded" in live.log_text(),
        f"loaded {loaded()}",
    )
    live.check(
        "the log shows the budget, the answer and the measurement",
        "budget 970 MiB on" in live.log_text() and "met (accepted=yes" in live.log_text(),
        next((l.strip()[-160:] for l in live.log_text().splitlines() if "budget 970 MiB" in l), "no line"),
    )
    live.call("/api/v1/unload", {"model_name": "m-accept"})

    for mode, why in (
        ("refuse", "reason: fixture refuses"),
        ("short", "not met"),
        ("lie", "not met"),
        ("garbage", "budget request failed"),
    ):
        name = "m-" + mode
        live.call("/api/v1/load", {"model_name": name})
        st, took = pressure(0.95)
        live.check(
            f"{mode}: asked, then unloaded exactly as before",
            len(budget_requests(name)) == 1 and name not in loaded() and why in live.log_text(),
            f"requests {len(budget_requests(name))}, loaded {loaded()}",
        )

    live.call("/api/v1/load", {"model_name": "m-slow"})
    result = {}

    def run():
        result["pressure"] = pressure(0.95)

    th = threading.Thread(target=run)
    th.start()
    time.sleep(1.5)  # the budget request is now waiting on the fixture
    t0 = time.time()
    st, h = live.call("/api/v1/health", timeout=30)
    health_took = time.time() - t0
    st2, models = live.call("/api/v1/models", timeout=30)
    th.join(120)
    live.check(
        "the router answers while the budget request is in flight (no router lock held)",
        st == 200 and st2 == 200 and health_took < 2.0,
        f"/health took {health_took:.2f}s during the request",
    )
    live.check(
        "a slow engine is given up on after the short timeout and unloaded",
        "m-slow" not in loaded() and result.get("pressure", (0, 99))[1] < 20,
        f"pressure evaluation took {result.get('pressure', (0, -1))[1]:.1f}s",
    )

    live.call("/api/v1/load", {"model_name": "m-plain"})
    st, took = pressure(0.95)
    live.check(
        "a spec without budget_path is unloaded with no request (unchanged)",
        "m-plain" not in loaded() and not os.path.exists(f"{T}/m-plain.rec.budget"),
        f"loaded {loaded()}",
    )

    st, body = live.call("/api/v1/load", {"model_name": "m-idle", "evict_idle_timeout": 1})
    time.sleep(2.5)
    st2, took = pressure(-1)
    live.check(
        "an idle-timeout eviction never asks",
        st == 200 and "m-idle" not in loaded() and not budget_requests("m-idle"),
        f"load {st} {body[:100]}, loaded {loaded()}, requests {budget_requests('m-idle')}",
    )
finally:
    sys.exit(live.finish("budget live"))
