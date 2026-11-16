"""L4 verdicts in tools/adapter_conformance.py: each failure says what was missing.

python3 -m unittest test/test_adapter_conformance_l4.py
"""

import os
import sys
import unittest

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
)
import adapter_conformance as ac  # noqa: E402


class TelemetryVerdict(unittest.TestCase):
    def test_engine_stats_pass(self):
        stats = {"engine_telemetry": {"m": {"adapter_stats": {"hits": 1}}}}
        self.assertTrue(ac.telemetry_verdict(stats, "m")[0])

    def test_memory_only_passes(self):
        stats = {"engine_telemetry": {"m": {"adapter_memory": {"local_bytes": 1}}}}
        self.assertTrue(ac.telemetry_verdict(stats, "m")[0])

    def test_public_name_prefix_matches(self):
        stats = {"engine_telemetry": {"user.m": {"adapter_stats": {}}}}
        self.assertTrue(ac.telemetry_verdict(stats, "m")[0])

    def test_no_hook_names_the_hook(self):
        ok, why = ac.telemetry_verdict({"requests": 1}, "m")
        self.assertFalse(ok)
        self.assertIn("router hook", why)
        self.assertNotIn("spec has no telemetry_path?", why)

    def test_missing_entry_names_the_spec_fields(self):
        ok, why = ac.telemetry_verdict({"engine_telemetry": {}}, "m")
        self.assertFalse(ok)
        self.assertIn("telemetry_path", why)

    def test_timeout_error_is_shown(self):
        stats = {"engine_telemetry": {"m": {"error": "telemetry timed out"}}}
        ok, why = ac.telemetry_verdict(stats, "m")
        self.assertFalse(ok)
        self.assertIn("timed out", why)

    def test_null_stats_means_unanswered(self):
        stats = {"engine_telemetry": {"m": {"adapter_stats": None}}}
        ok, why = ac.telemetry_verdict(stats, "m")
        self.assertFalse(ok)
        self.assertIn("did not answer", why)


if __name__ == "__main__":
    unittest.main()
