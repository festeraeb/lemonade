"""Shared setup for the adapter plane's live tests: a throwaway tree (adapters folder, models folder,
lemond home) in a temp folder, a lemond started on 127.0.0.1:$ADAPTER_LIVE_PORT with that home, and a
small HTTP helper. Nothing here touches a GPU: lemond is started with a PATH that holds no GPU tools,
so its VRAM monitor reads nothing and only simulated pressure reaches the eviction engine."""

import json, os, shutil, signal, subprocess, sys, tempfile, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = HERE + "/fake_engine.py"


class Live:
    def __init__(self, lemond, port_default):
        self.lemond = os.path.abspath(lemond)
        self.port = int(os.environ.get("ADAPTER_LIVE_PORT", str(port_default)))
        self.t = tempfile.mkdtemp(prefix="adapter-live-")
        for d in ("adapters", "models", "bin", "home/.config/lemonade", "home/.cache/lemonade"):
            os.makedirs(f"{self.t}/{d}")
        shutil.copy(FIXTURE, self.t + "/fake_engine.py")
        os.chmod(self.t + "/fake_engine.py", 0o755)
        # A PATH with python3 and nothing else: no nvidia-smi or rocm-smi reaches lemond.
        os.symlink(sys.executable, self.t + "/bin/python3")
        self.results = []
        self.p = None

    def spec(self, name, spec):
        with open(f"{self.t}/adapters/{name}.json", "w") as f:
            if isinstance(spec, str):
                f.write(spec)
            else:
                json.dump(spec, f, indent=1)

    def user_models(self, rows):
        with open(self.t + "/home/.config/lemonade/user_models.json", "w") as f:
            json.dump(rows, f, indent=1)

    def start(self, extra_env=None):
        env = dict(
            os.environ,
            HOME=self.t + "/home",
            XDG_CONFIG_HOME=self.t + "/home/.config",
            XDG_CACHE_HOME=self.t + "/home/.cache",
            LEMONADE_CACHE_DIR=self.t + "/home/.cache/lemonade",
            LEMONADE_ADAPTERS_DIR=self.t + "/adapters",
            PATH=self.t + "/bin",
            CUDA_VISIBLE_DEVICES="-1",
        )
        env.update(extra_env or {})
        self.log = open(self.t + "/lemond.log", "w")
        self.p = subprocess.Popen(
            [self.lemond, "--host", "127.0.0.1", "--port", str(self.port)],
            env=env,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            cwd=self.t,
            start_new_session=True,
        )
        for _ in range(60):
            try:
                if self.call("/api/v1/health", timeout=3)[0] == 200:
                    return
            except Exception:
                pass
            time.sleep(1)
        raise RuntimeError("lemond did not answer /api/v1/health")

    def call(self, path, body=None, timeout=60, method=None):
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}",
            data=json.dumps(body).encode() if body is not None else None,
            headers={"content-type": "application/json"},
            method=method,
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def log_text(self):
        with open(self.t + "/lemond.log", errors="replace") as f:
            return f.read()

    def check(self, name, ok, detail):
        self.results.append(bool(ok))
        print(("PASS " if ok else "FAIL ") + name + ": " + detail, flush=True)

    def finish(self, title):
        if self.p is not None:
            try:
                os.killpg(self.p.pid, signal.SIGTERM)
                self.p.wait(15)
            except Exception:
                try:
                    os.killpg(self.p.pid, signal.SIGKILL)
                except Exception:
                    pass
        print(f"{title}: {sum(self.results)}/{len(self.results)} passed", flush=True)
        if os.environ.get("ADAPTER_LIVE_KEEP"):
            print("kept " + self.t)
        else:
            shutil.rmtree(self.t, ignore_errors=True)
        return 0 if self.results and all(self.results) else 1


def find_model(models_json, model_id):
    for m in json.loads(models_json).get("data", []):
        if m.get("id") == model_id:
            return m
    return None
