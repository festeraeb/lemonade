#!/usr/bin/env python3
"""Live check of the adapter plane's spec wiring against a built lemond.

    python3 test/cpp/adapter_spec/live/spec_wiring_live.py <path to lemond>

Builds a throwaway tree (adapters, model file, lemond home) in a temp folder, starts lemond on
127.0.0.1:13807, loads a fake engine that records the argv and environment it was given, and checks
that specs with a forbidden variable, an unknown placeholder, a library path outside the engine, or
no binary are refused with a message naming the problem."""
import json, os, shutil, signal, subprocess, sys, tempfile, time, urllib.request, urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
T = tempfile.mkdtemp(prefix="adapter-live-")
os.makedirs(T + "/adapters"); os.makedirs(T + "/home/.config/lemonade"); os.makedirs(T + "/home/.cache/lemonade"); os.makedirs(T + "/models")
shutil.copy(HERE + "/fake_engine.py", T + "/fake_engine.py"); os.chmod(T + "/fake_engine.py", 0o755)
open(T + "/models/model with space.gguf", "w").write("x")
base = {"spec_version": 1, "binary": T + "/fake_engine.py", "ready": {"path": "/health", "timeout_s": 30},
        "env": {"CUDA_VISIBLE_DEVICES": "{device}", "LEMONADE_ENGINE_RECORD": T + "/record-{device}.json", "LD_LIBRARY_PATH": "{spec_dir}/lib"},
        "default_device_index": "0", "display_name": "fake (extra fields are ignored)"}
specs = {
    "fake-good": dict(base, args=["--port", "{port}", "--model", "{model_path}", "--cfg", "{}"]),
    "fake-preload": dict(base, args=["--port", "{port}"], env=dict(base["env"], LD_PRELOAD="/tmp/evil.so")),
    "fake-badph": dict(base, args=["--port", "{port}", "--who", "{shell}"]),
    "fake-libpath": dict(base, args=["--port", "{port}"], env=dict(base["env"], LD_LIBRARY_PATH="/tmp/evil")),
    "fake-nobinary": {k: v for k, v in dict(base, args=["--port", "{port}"]).items() if k != "binary"},
}
for n, s in specs.items():
    json.dump(s, open(f"{T}/adapters/{n}.json", "w"), indent=1)
json.dump({n: {"checkpoint": f"{n}::{T}/models/model with space.gguf", "labels": ["chat", "custom"], "recipe": "adapter", "size": 0.01}
           for n in specs}, open(T + "/home/.config/lemonade/user_models.json", "w"), indent=1)
PORT = 13807
lemond = sys.argv[1]
env = dict(os.environ, HOME=T + "/home", XDG_CONFIG_HOME=T + "/home/.config", XDG_CACHE_HOME=T + "/home/.cache",
           LEMONADE_CACHE_DIR=T + "/home/.cache/lemonade", LEMONADE_ADAPTERS_DIR=T + "/adapters")
log = open(T + "/lemond.log", "w")
p = subprocess.Popen([lemond, "--host", "127.0.0.1", "--port", str(PORT)], env=env, stdout=log, stderr=subprocess.STDOUT,
                     cwd=T, start_new_session=True)

def call(path, body=None, timeout=60):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=json.dumps(body).encode() if body is not None else None,
                                 headers={"content-type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()

results = []
def check(name, ok, detail):
    results.append(ok); print(("PASS " if ok else "FAIL ") + name + ": " + detail)

try:
    for _ in range(60):
        try:
            if call("/api/v1/health", timeout=3)[0] == 200: break
        except Exception: pass
        time.sleep(1)
    rec = T + "/record-0.json"
    if os.path.exists(rec): os.remove(rec)
    st, body = call("/api/v1/load", {"model_name": "fake-good"})
    r = json.load(open(rec)) if os.path.exists(rec) else {}
    argv = r.get("argv", [])
    check("good spec loads", st == 200, f"status {st}")
    check("argv filled, one entry per element", argv[2:] == ["--model", T + "/models/model with space.gguf", "--cfg", "{}"] and argv[:1] == ["--port"],
          f"argv {argv}")
    check("env filled", r.get("env", {}).get("CUDA_VISIBLE_DEVICES") == "0" and r.get("env", {}).get("LD_LIBRARY_PATH") == T + "/adapters/lib",
          f"env {r.get('env')}")
    st, chat = call("/api/v1/chat/completions", {"model": "fake-good", "messages": [{"role": "user", "content": "hi"}]})
    check("good spec answers", st == 200 and "fake engine answer" in chat, f"status {st}")
    call("/api/v1/unload", {"model_name": "fake-good"})
    for model, needle in (("fake-preload", "LD_PRELOAD"), ("fake-badph", "{shell}"), ("fake-libpath", "/tmp/evil"), ("fake-nobinary", "binary")):
        st, body = call("/api/v1/load", {"model_name": model})
        check(f"{model} refused naming {needle}", st >= 400 and needle in body, f"status {st}: {body[:160]}")
finally:
    os.killpg(p.pid, signal.SIGTERM)
    try: p.wait(10)
    except Exception: os.killpg(p.pid, signal.SIGKILL)
print(f"spec wiring live: {sum(results)}/{len(results)} passed")
shutil.rmtree(T, ignore_errors=True)
sys.exit(0 if all(results) else 1)
