#!/usr/bin/env python3
"""Conformance test for one engine adapter: which control level does it actually earn?

Runs against a running lemond that has the adapter backend and the adapter's spec installed. Each level is
tested in order and the adapter is given the highest level that passes, which can be lower than the level its
spec claims (a downgrade, not a failure). Level 0 needs no test: the engine can always be run without control
(registered as a provider), so a failed adapter never takes the engine away.

    L1 lifecycle   lemond starts the engine on load, it answers a chat, and unload frees its GPU memory
                   (checked on the device with nvidia-smi, not taken from the engine's word)
    L2 eviction    with max_models llm=1, loading another model evicts this one and frees its memory
    L3 options     a per-model device option is honoured: the engine runs on the GPU it was given
    L4 telemetry   the spec's telemetry_path answers through lemond's telemetry

    python tools/adapter_conformance.py --lemond http://127.0.0.1:13806 --model <adapter model> \
        [--other <a second adapter model, for L2>] [--device <GPU index for L3>] [--json]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import urllib.request

PROMPT = "In one sentence, what is a lighthouse for?"


def call(base: str, path: str, body: dict | None = None, timeout: int = 1900) -> dict:
    req = urllib.request.Request(base + path, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read() or b"{}")


def loaded(base: str) -> list[str]:
    return [m.get("model_name") for m in call(base, "/api/v1/health", timeout=10).get("all_models_loaded", [])]


def gpu_mib() -> dict[int, int]:
    """GPU index -> MiB used. Empty when nvidia-smi is not installed (the GPU checks then cannot pass)."""
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=index,memory.used", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return {}
    return {int(i): int(m) for i, m in (line.split(",") for line in out.strip().splitlines() if "," in line)}


def chat(base: str, model: str) -> str:
    r = call(base, "/api/v1/chat/completions",
             {"model": model, "max_tokens": 60, "messages": [{"role": "user", "content": PROMPT}]})
    return (r.get("choices") or [{}])[0].get("message", {}).get("content", "")


def wait_freed(before: dict[int, int], slack_mib: int = 300, secs: int = 20) -> bool:
    """True once every GPU is back within slack of its use before the engine loaded."""
    for _ in range(secs):
        now = gpu_mib()
        if now and all(now.get(i, 0) <= used + slack_mib for i, used in before.items()):
            return True
        time.sleep(1)
    return False


def run(base: str, model: str, other: str | None, device: int | None) -> dict:
    report = {"model": model, "levels": {}, "earned": 0, "notes": []}

    def result(level: int, ok: bool, detail: str) -> bool:
        report["levels"][f"L{level}"] = {"pass": ok, "detail": detail}
        if ok and report["earned"] == level - 1:
            report["earned"] = level
        return ok

    if model in loaded(base):
        call(base, "/api/v1/unload", {"model_name": model}, timeout=120)
        time.sleep(3)
    idle = gpu_mib()

    # L1: start, answer, unload frees the device
    t0 = time.time()
    try:
        answer = chat(base, model)
    except Exception as e:  # noqa: BLE001 - any failure here means no lifecycle control
        result(1, False, f"load/chat failed: {e}")
        report["notes"].append("falls back to L0: run the engine without control")
        return report
    secs = round(time.time() - t0, 1)
    held = {i: m - idle.get(i, 0) for i, m in gpu_mib().items() if m - idle.get(i, 0) > 300}
    call(base, "/api/v1/unload", {"model_name": model}, timeout=120)
    freed = wait_freed(idle)
    ok = bool(answer.strip()) and model not in loaded(base) and freed
    result(1, ok, f"answered in {secs}s ({answer.strip()[:80]!r}); held {held or 'no GPU memory'}; "
                  f"after unload {'freed' if freed else 'NOT freed'}")
    if not ok:
        report["notes"].append("falls back to L0: run the engine without control")
        return report

    # L2: eviction under max_models llm=1
    if other:
        chat(base, model)
        chat(base, other)
        evicted = model not in loaded(base)
        call(base, "/api/v1/unload", {"model_name": other}, timeout=120)
        freed = wait_freed(idle)
        result(2, evicted and freed, f"loading {other} {'evicted' if evicted else 'did NOT evict'} {model}; "
                                     f"memory {'freed' if freed else 'NOT freed'} after both")
    else:
        result(2, False, "not tested: pass --other <second adapter model>")

    # L3: the device option is honoured
    if device is not None:
        try:
            call(base, "/api/v1/load", {"model_name": model, "adapter_device": str(device)}, timeout=1900)
            used = {i: m - idle.get(i, 0) for i, m in gpu_mib().items() if m - idle.get(i, 0) > 300}
            on_it = device in used and all(i == device for i in used)
            result(3, on_it, f"asked for GPU {device}; memory on {sorted(used) or 'no GPU'}")
        except Exception as e:  # noqa: BLE001
            result(3, False, f"load with adapter_device failed: {e}")
        finally:
            call(base, "/api/v1/unload", {"model_name": model}, timeout=120)
            wait_freed(idle)
    else:
        result(3, False, "not tested: pass --device <GPU index>")

    # L4: telemetry through lemond
    try:
        chat(base, model)
        stats = call(base, "/api/v1/stats", timeout=20)
        has = "adapter_stats" in json.dumps(stats)
        result(4, has, "adapter_stats served by lemond" if has else "no adapter_stats (spec has no telemetry_path?)")
    except Exception as e:  # noqa: BLE001
        result(4, False, f"stats failed: {e}")
    finally:
        call(base, "/api/v1/unload", {"model_name": model}, timeout=120)
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lemond", default="http://127.0.0.1:13305")
    ap.add_argument("--model", required=True)
    ap.add_argument("--other", default=None)
    ap.add_argument("--device", type=int, default=None)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    report = run(a.lemond.rstrip("/"), a.model, a.other, a.device)
    if a.json:
        print(json.dumps(report, indent=2))
    else:
        for lvl, r in report["levels"].items():
            print(f"{lvl}: {'pass' if r['pass'] else 'FAIL'}  {r['detail']}")
        print(f"earned level: L{report['earned']}" + "".join(f"\n  {n}" for n in report["notes"]))
    return 0 if report["earned"] >= 1 else 1


if __name__ == "__main__":
    sys.exit(main())
