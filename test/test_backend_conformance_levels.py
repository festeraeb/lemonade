"""Level grading in tools/backend_conformance.py against a scripted lemond.

python3 -m unittest test/test_backend_conformance_levels.py
"""

import sys
import unittest
import urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import backend_conformance as bc  # noqa: E402

BASE = "http://127.0.0.1:13305"
METRICS = (
    "# TYPE lemonade_llamacpp_tokens_predicted_total counter\n"
    'lemonade_llamacpp_tokens_predicted_total{model_name="m",recipe="llamacpp"} 60\n'
    'lemonade_llamacpp_requests_processing{model_name="m",recipe="llamacpp"} 0\n'
    'lemonade_model_loaded{model_name="m"} 1\n'
)


def llamacpp_facts():
    builtin = bc.read_sources(bc.BUILTIN_DIR)
    return bc.facts(bc.resolve("llamacpp", builtin, {}, "builtin"))


class FakeLemond:
    """One loaded model at a time, engines as fake pids, GPU memory per device."""

    def __init__(self, info, recipe="llamacpp", model_recipe="llamacpp"):
        self.info = info
        self.recipe = recipe
        self.model_recipe = model_recipe
        self.definition_hash = info["hash"]
        self.definition_id = info["id"]
        self.metrics = METRICS
        self.loaded = {}
        self.pids = set()
        self.survivors = set()
        self.gpus = None
        self.next_pid = 1000

    def gpu_mib(self):
        if self.gpus is None:
            return {}
        used = dict(self.gpus)
        for _pid, device in self.loaded.values():
            if device and device[-1].isdigit():
                used[int(device[-1])] += 2000
        return used

    def load(self, model, device=None):
        if model in self.loaded and self.loaded[model][1] == device:
            return
        for name, (pid, _device) in list(self.loaded.items()):
            del self.loaded[name]
            if pid not in self.survivors:
                self.pids.discard(pid)
        self.next_pid += 1
        self.loaded[model] = (self.next_pid, device)
        self.pids.add(self.next_pid)

    def health(self):
        rows = []
        for name, (pid, device) in self.loaded.items():
            argv = ["llama-server", "--port", "8001"]
            if device:
                argv += ["--device", device]
            rows.append({"model_name": name, "pid": pid, "launch_command": argv})
        return {"all_models_loaded": rows}

    def __call__(self, base, path, body=None, timeout=1900):
        if path.startswith("/api/v1/backends/schema"):
            if self.recipe is None:
                raise urllib.error.HTTPError(
                    base + path, 404, "unknown_recipe", {}, None
                )
            row = {"recipe": self.recipe, "definition_hash": self.definition_hash}
            row["definition_id"] = self.definition_id
            return {"recipes": [row]}
        if path.startswith("/api/v1/models"):
            return {
                "data": [{"id": n, "recipe": self.model_recipe} for n in ("m", "o")]
            }
        if path == "/api/v1/health":
            return self.health()
        if path == "/api/v1/chat/completions":
            self.load(body["model"])
            return {
                "choices": [{"message": {"content": "", "reasoning_content": "Ships."}}]
            }
        if path == "/api/v1/load":
            self.load(body["model_name"], body.get(self.info["device_option"]))
            return {"status": "success"}
        if path == "/api/v1/unload":
            pid, _device = self.loaded.pop(body["model_name"])
            if pid not in self.survivors:
                self.pids.discard(pid)
            return {"status": "success"}
        if path == "/api/v1/stats":
            return {"request_count_total": 1}
        if path == "/metrics":
            return self.metrics
        raise AssertionError(path)


class Levels(unittest.TestCase):
    def setUp(self):
        self.saved = {
            name: getattr(bc, name)
            for name in ("call", "gpu_mib", "pid_alive", "rss_mib")
        }
        self.info = llamacpp_facts()
        self.fake = FakeLemond(self.info)
        bc.call = self.fake
        bc.gpu_mib = self.fake.gpu_mib
        bc.pid_alive = lambda pid: pid in self.fake.pids
        bc.rss_mib = lambda pid: 500

    def tearDown(self):
        for name, value in self.saved.items():
            setattr(bc, name, value)

    def grade(
        self, other="o", device="none", device_gpu=None, cpu_only=True, info=None
    ):
        return bc.run(BASE, info or self.info, "m", other, device, device_gpu, cpu_only)

    def status(self, report):
        return {k: v["status"] for k, v in report["levels"].items()}

    def test_cpu_run_stops_at_the_untested_device_level(self):
        report = self.grade()
        self.assertEqual(
            self.status(report),
            {"L1": bc.PASS, "L2": bc.PASS, "L3": bc.NOT_TESTED, "L4": bc.PASS},
        )
        self.assertEqual(report["earned"], 2)
        self.assertIn("argv has --device none", report["levels"]["L3"]["detail"])
        self.assertIn("lemond runs this definition", report["catalog"])
        self.assertTrue(any("engine_telemetry" in n for n in report["notes"]))
        self.assertFalse(self.fake.loaded)

    def test_gpu_run_earns_l4_when_memory_lands_on_the_asked_card(self):
        self.fake.gpus = {0: 100, 1: 100}
        report = self.grade(device="CUDA1", cpu_only=False)
        self.assertEqual(report["levels"]["L3"]["status"], bc.PASS)
        self.assertIn("memory on [1]", report["levels"]["L3"]["detail"])
        self.assertEqual(report["earned"], 4)

    def test_gpu_run_fails_l3_when_memory_lands_elsewhere(self):
        self.fake.gpus = {0: 100, 1: 100}
        report = self.grade(device="CUDA0", device_gpu=1, cpu_only=False)
        self.assertEqual(report["levels"]["L3"]["status"], bc.FAIL)
        self.assertEqual(report["earned"], 2)

    def test_engine_left_running_fails_l1(self):
        self.fake.survivors = set(range(1000, 1100))
        bc.wait_gone, saved = (lambda pid, secs=20: not bc.pid_alive(pid)), bc.wait_gone
        try:
            report = self.grade()
        finally:
            bc.wait_gone = saved
        self.assertEqual(report["levels"]["L1"]["status"], bc.FAIL)
        self.assertIn("still running", report["levels"]["L1"]["detail"])
        self.assertEqual(report["earned"], 0)

    def test_missing_recipe_means_not_installed(self):
        self.fake.recipe = None
        report = self.grade()
        self.assertEqual(report["earned"], 0)
        self.assertIn("not installed", report["levels"]["L1"]["detail"])

    def test_model_of_another_recipe_is_refused(self):
        self.fake.model_recipe = "whispercpp"
        report = self.grade()
        self.assertEqual(report["levels"]["L1"]["status"], bc.FAIL)
        self.assertIn("whispercpp", report["levels"]["L1"]["detail"])

    def test_changed_definition_is_reported(self):
        self.fake.definition_hash = "sha256:" + "0" * 64
        self.assertIn("differs", self.grade()["catalog"])

    def test_definition_sharing_a_recipe_is_named(self):
        self.fake.definition_hash = "sha256:" + "0" * 64
        self.fake.definition_id = "other"
        self.assertIn("'other'", self.grade()["catalog"])

    def test_no_other_model_leaves_l2_untested(self):
        report = self.grade(other=None)
        self.assertEqual(report["levels"]["L2"]["status"], bc.NOT_TESTED)
        self.assertEqual(report["earned"], 1)

    def test_definition_without_device_option(self):
        builtin = bc.read_sources(bc.BUILTIN_DIR)
        info = bc.facts(bc.resolve("hrx", builtin, {}, "builtin"))
        self.fake.recipe = self.fake.model_recipe = info["recipe"]
        self.fake.definition_hash = info["hash"]
        report = self.grade(info=info)
        self.assertEqual(report["levels"]["L3"]["status"], bc.NOT_GRADABLE)
        self.assertIn("no device option", report["levels"]["L3"]["detail"])

    def test_definition_without_metrics(self):
        info = dict(self.info, metrics=False)
        report = self.grade(info=info)
        self.assertEqual(report["levels"]["L4"]["status"], bc.NOT_GRADABLE)
        self.assertIn("exposes_prometheus_metrics", report["levels"]["L4"]["detail"])


class MetricsVerdict(unittest.TestCase):
    def test_engine_samples_for_the_model_pass(self):
        ok, why = bc.metrics_verdict(METRICS, "m")
        self.assertTrue(ok)
        self.assertIn("2 engine metric families", why)

    def test_canonical_name_matches(self):
        text = METRICS.replace('model_name="m"', 'model_name="user.m"')
        self.assertTrue(bc.metrics_verdict(text, "m")[0])

    def test_lemonade_own_samples_and_other_models_do_not_count(self):
        text = 'lemonade_model_loaded{model_name="m"} 1\n' + METRICS.replace(
            '"m"', '"x"'
        )
        ok, why = bc.metrics_verdict(text, "m")
        self.assertFalse(ok)
        self.assertIn("model_name='m'", why)


if __name__ == "__main__":
    unittest.main()
