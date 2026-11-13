#!/usr/bin/env python3
"""Live check of models declared by a spec ("models" array) against a built lemond.

    python3 test/cpp/adapter_spec/live/declared_models_live.py <path to lemond>

Starts lemond on 127.0.0.1:$ADAPTER_LIVE_PORT (default 13811) with a throwaway home whose
user_models.json registers nothing adapter-related except one deliberate name clash. Checks that the
spec's models are listed without a registration, that a missing file is listed as not downloaded,
that malformed entries are skipped with a log line naming the spec and field, that a declared name
cannot shadow a built-in model or a registration, and that a declared model loads and answers through
the fixture engine (fake_engine.py, a test stand-in, not a real engine)."""

import json, os, sys
from harness import Live, find_model

live = Live(sys.argv[1], 13811)
T = live.t
os.makedirs(T + "/adapters/weights")
open(T + "/models/good.gguf", "w").write("x")
open(T + "/adapters/weights/rel.gguf", "w").write("x")
open(T + "/models/user.gguf", "w").write("x")
BUILTIN = "MiniCPM5-1B-GGUF"
live.spec(
    "fake",
    {
        "spec_version": 1,
        "binary": T + "/fake_engine.py",
        "args": ["--port", "{port}", "--model", "{model_path}", "--ctx", "{ctx}"],
        "env": {"LEMONADE_ENGINE_RECORD": T + "/record.json"},
        "ready": {"path": "/health", "timeout_s": 30},
        "models": [
            {"name": "decl-good", "path": T + "/models/good.gguf", "labels": ["chat"], "ctx": 3072},
            {"name": "decl-rel", "path": "weights/rel.gguf"},
            {"name": "decl-missing", "path": T + "/models/not-there.gguf"},
            {"name": "user.spoof", "path": T + "/models/good.gguf"},
            {"name": "decl-nopath"},
            {"name": "decl-badctx", "path": T + "/models/good.gguf", "ctx": "big"},
            {"name": BUILTIN, "path": T + "/models/good.gguf"},
            {"name": "mine", "path": T + "/models/good.gguf"},
        ],
    },
)
live.spec("broken", "{ this is not json")
live.user_models(
    {
        "mine": {
            "checkpoint": "fake::" + T + "/models/user.gguf",
            "recipe": "adapter",
            "labels": ["chat"],
            "size": 0.01,
        }
    }
)

try:
    live.start()
    st, all_models = live.call("/api/v1/models?show_all=true")
    st2, listed = live.call("/api/v1/models")
    good = find_model(all_models, "decl-good") or {}
    live.check(
        "a declared model is listed without a registration",
        good.get("recipe") == "adapter" and good.get("checkpoint") == "fake::" + T + "/models/good.gguf",
        f"decl-good {json.dumps(good)[:300]}",
    )
    live.check(
        "it is downloaded and in the default list",
        good.get("downloaded") is True and find_model(listed, "decl-good") is not None,
        f"downloaded={good.get('downloaded')}",
    )
    rel = find_model(all_models, "decl-rel") or {}
    live.check(
        "a relative path is read from the spec folder",
        rel.get("checkpoint") == "fake::" + T + "/adapters/weights/rel.gguf",
        f"checkpoint {rel.get('checkpoint')}",
    )
    missing = find_model(all_models, "decl-missing") or {}
    live.check(
        "a missing file is listed as not downloaded, as for a registration",
        missing.get("downloaded") is False and find_model(listed, "decl-missing") is None,
        f"decl-missing {json.dumps(missing)[:200]}",
    )
    log = live.log_text()
    ids = [m.get("id") for m in json.loads(all_models).get("data", [])]
    live.check(
        "malformed entries are skipped with the spec and field named",
        "user.spoof" not in ids
        and "decl-nopath" not in ids
        and "decl-badctx" not in ids
        and "adapter spec 'fake': models[3].name" in log
        and "adapter spec 'fake': models[4].path" in log
        and "adapter spec 'fake': models[5].ctx" in log,
        "log lines: " + " | ".join(l.strip()[-90:] for l in log.splitlines() if "adapter spec 'fake'" in l)[:400],
    )
    live.check(
        "a broken spec file is named and costs only itself",
        "adapter spec 'broken': not valid JSON" in log,
        "broken spec logged" if "'broken'" in log else "no log line",
    )
    bi = find_model(all_models, BUILTIN) or {}
    live.check(
        "a declared name cannot shadow a built-in model",
        bi.get("recipe") not in (None, "adapter") or (bi == {} and f"Declared model '{BUILTIN}'" in log),
        f"{BUILTIN}: recipe {bi.get('recipe')}",
    )
    mine = find_model(all_models, "mine") or {}
    live.check(
        "a registration keeps its name",
        mine.get("checkpoint") == "fake::" + T + "/models/user.gguf"
        and "Declared model 'mine'" in log,
        f"mine -> {mine.get('checkpoint')}",
    )
    st, body = live.call("/api/v1/load", {"model_name": "decl-good"})
    rec = json.load(open(T + "/record.json")) if os.path.exists(T + "/record.json") else {}
    live.check(
        "ctx is the model's context window",
        good.get("max_context_window") == 3072,
        f"max_context_window {good.get('max_context_window')}",
    )
    live.check(
        "the declared model loads and {ctx} is filled",
        st == 200 and rec.get("argv", [])[-2:-1] == ["--ctx"] and rec["argv"][-1].isdigit(),
        f"status {st}, argv {rec.get('argv')}",
    )
    st, chat = live.call(
        "/api/v1/chat/completions",
        {"model": "decl-good", "messages": [{"role": "user", "content": "hi"}]},
    )
    live.check("it answers", st == 200 and "fake engine answer" in chat, f"status {st}")
    live.call("/api/v1/unload", {"model_name": "decl-good"})
    st, body = live.call("/api/v1/delete", {"model_name": "decl-good"})
    live.check(
        "deleting it never removes the file",
        os.path.exists(T + "/models/good.gguf"),
        f"delete status {st}: {body[:160]}",
    )
finally:
    sys.exit(live.finish("declared models live"))
