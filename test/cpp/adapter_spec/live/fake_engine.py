#!/usr/bin/env python3
"""TEST FIXTURE, not an engine. A stand-in for the adapter plane's live tests: records the argv and
environment it was started with (to $LEMONADE_ENGINE_RECORD), then serves /health, /v1/models, a one-line
chat, a memory report and a budget route. Its numbers are made up by the test that starts it and say
nothing about any real engine.

Knobs, all through the environment the spec sets (names pass the adapter's allowlist):
  LEMONADE_ENGINE_MEM_MIB  bytes it reports holding on its device, in MiB (default: 1 MiB)
  LEMONADE_ENGINE_UUID     device id in its memory report (default GPU-fake)
  LEMONADE_ENGINE_BUDGET   how POST /budget answers: accept (default), refuse, short (frees less than
                           asked), lie (says accepted, keeps the memory), garbage (not JSON), slow (60 s)
POST /fixture/hang makes /health and chat stop answering, as a hung engine would. Every budget request
is appended to $LEMONADE_ENGINE_RECORD.budget.
"""

import json, os, sys, threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MIB = 1024 * 1024
port = int(sys.argv[sys.argv.index("--port") + 1])
record = os.environ["LEMONADE_ENGINE_RECORD"]
with open(record, "w") as f:
    json.dump(
        {
            "argv": sys.argv[1:],
            "env": {
                k: v
                for k, v in os.environ.items()
                if k.startswith(
                    (
                        "CUDA_",
                        "GGML_",
                        "LEMONADE_ENGINE_",
                        "LD_LIBRARY_PATH",
                        "LD_PRELOAD",
                        "STRATA_",
                    )
                )
            },
        },
        f,
    )

state = {
    "bytes": int(float(os.environ.get("LEMONADE_ENGINE_MEM_MIB", "1")) * MIB),
    "uuid": os.environ.get("LEMONADE_ENGINE_UUID", "GPU-fake"),
    "budget": os.environ.get("LEMONADE_ENGINE_BUDGET", "accept"),
    "hung": False,
}
lock = threading.Lock()


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _j(self, obj, raw=None):
        b = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def _hang_if_hung(self):
        if state["hung"]:
            time.sleep(60)

    def do_GET(self):
        if self.path.startswith("/hang"):  # a stuck telemetry route
            time.sleep(60)
            return self._j({})
        if self.path.startswith("/memory"):  # R9: memory by device and tier
            with lock:
                held = state["bytes"]
            return self._j(
                {
                    "devices": [
                        {"uuid": state["uuid"], "bytes": held, "evictable": True}
                    ],
                    "pinned_host": 2048,
                    "remote": 4096,
                    "disk": 8192,
                }
            )
        if self.path.startswith("/health"):
            self._hang_if_hung()
            return self._j({"status": "ok"})
        self._j(
            {
                "object": "list",
                "data": [{"id": "fake", "object": "model", "owned_by": "fake"}],
            }
        )

    def do_POST(self):
        n = int(self.headers.get("content-length") or 0)
        body = self.rfile.read(n)
        if self.path.startswith("/fixture/hang"):
            state["hung"] = True
            return self._j({"hung": True})
        if self.path.startswith("/budget"):
            return self._budget(json.loads(body or b"{}"))
        self._hang_if_hung()
        self._j(
            {
                "id": "x",
                "object": "chat.completion",
                "model": "fake",
                "choices": [
                    {
                        "index": 0,
                        "finish_reason": "stop",
                        "message": {
                            "role": "assistant",
                            "content": "fake engine answer",
                        },
                    }
                ],
            }
        )

    def _budget(self, req):
        with open(record + ".budget", "a") as f:
            f.write(json.dumps(req) + "\n")
        mode, want = state["budget"], int(req.get("mib", 0)) * MIB
        if mode == "slow":
            time.sleep(60)
        if mode == "garbage":
            return self._j(None, raw=b"this is not json")
        if mode == "refuse":
            return self._j({"accepted": False, "reason": "fixture refuses"})
        with lock:
            if mode == "accept":
                state["bytes"] = want
            elif mode == "short":
                state["bytes"] = want + 512 * MIB
            reply_bytes = want if mode == "lie" else state["bytes"]
        return self._j({"accepted": True, "now_bytes": reply_bytes})


ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
