"""Definition reading in tools/backend_conformance.py: resolution must match lemond's own.

python3 -m unittest test/test_backend_conformance_definitions.py
"""

import hashlib
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import backend_conformance as bc  # noqa: E402

EXTERNAL = ROOT / "test" / "fixtures" / "backend-v2" / "external"
CONFIGURED = ROOT / "src" / "cpp" / "server" / "backends" / "configured"
PIN_RE = re.compile(
    r'BuiltinExportContract \w+\{\{"([a-z][a-z0-9._-]*)", \d+\},\s*'
    r'"(sha256:[0-9a-f]{64})",\s*"(sha256:[0-9a-f]{64})"'
)


def builtin_pins():
    pins = {}
    for name in ("backend_llamacpp_export.cpp", "backend_llamacpp_policy.cpp"):
        for key, _source, definition in PIN_RE.findall((CONFIGURED / name).read_text()):
            pins[key] = definition
    return pins


class MatchesLemond(unittest.TestCase):
    def setUp(self):
        self.builtin = bc.read_sources(bc.BUILTIN_DIR)
        self.external = bc.read_sources(EXTERNAL)

    def test_resolved_hash_equals_the_compiled_pins(self):
        pins = builtin_pins()
        self.assertIn("llamacpp", pins)
        self.assertIn("hrx", pins)
        for key, expected in pins.items():
            declaration = bc.resolve(key, self.builtin, self.external, "builtin")
            self.assertEqual(bc.definition_hash(declaration), expected, key)

    def test_llamacpp_facts(self):
        f = bc.facts(bc.resolve("llamacpp", self.builtin, {}, "builtin"))
        self.assertEqual(f["recipe"], "llamacpp")
        self.assertEqual(f["readiness"]["path"], "/health")
        self.assertEqual(f["device_option"], "llamacpp_device")
        self.assertTrue(f["device_bound"])
        self.assertTrue(f["metrics"])
        self.assertTrue(f["chat"])

    def test_hrx_inherits_but_removes_the_device_option(self):
        f = bc.facts(bc.resolve("hrx", self.builtin, {}, "builtin"))
        self.assertEqual(f["recipe"], "llamacpp-hrx")
        self.assertIsNone(f["device_option"])
        self.assertTrue(f["metrics"])

    def test_external_child_of_builtin(self):
        d = bc.resolve("research-llama", self.builtin, self.external)
        f = bc.facts(d)
        self.assertEqual(f["device_option"], "research_llama_device")
        self.assertTrue(f["device_bound"])
        self.assertEqual([r["id"] for r in d["metadata"]["support"]], ["cpu-linux-x64"])
        self.assertNotIn("environment", d["hooks"])
        self.assertIn("launch", d["hooks"])
        self.assertEqual(d["launch"]["environment"]["preset"], "inherit")
        self.assertEqual(
            d["launch"]["environment"]["set"][-1]["name"], "GGML_DISABLE_VULKAN"
        )
        self.assertNotIn("cli_flag", d["metadata"]["options"][0])
        self.assertEqual(len(d["operations"]), 12)

    def test_v1_definition_is_its_own_declaration(self):
        d = bc.resolve("kokoro", self.builtin, {}, "builtin")
        self.assertEqual(d, self.builtin["kokoro"][1])
        compact = json.dumps(
            d, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        )
        self.assertEqual(
            bc.definition_hash(d),
            "sha256:" + hashlib.sha256(compact.encode()).hexdigest(),
        )

    def test_definition_found_by_builtin_path(self):
        path = str(bc.BUILTIN_DIR / "llamacpp.json")
        self.assertEqual(
            bc.find_definition(path, self.builtin, {}), ("llamacpp", "builtin")
        )

    def test_definition_found_by_new_path_is_external(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "x.json")
            Path(path).write_text(
                json.dumps({"schema_version": 2, "id": "x", "recipe": "x"})
            )
            external = {}
            self.assertEqual(bc.find_definition(path, {}, external), ("x", "external"))
            self.assertIn("x", external)

    def test_unknown_definition_is_named(self):
        with self.assertRaises(bc.DefinitionError) as e:
            bc.find_definition("nope", self.builtin, {})
        self.assertIn("nope", str(e.exception))


def leaf(edits, merge=None, parent="base"):
    return {
        "schema_version": 2,
        "id": "leaf",
        "recipe": "leaf",
        "extends": {"external": parent},
        "merge": merge or {},
        "edits": edits,
    }


BASE = {
    "schema_version": 2,
    "id": "base",
    "recipe": "base",
    "merge": {
        "kind": "local",
        "metadata": {"display_name": "Base", "options": [{"id": "a"}, {"id": "b"}]},
        "launch": {"argv": [{"id": "x", "flag": "-x"}], "environment": {"set": []}},
    },
}


def resolved(edits, merge=None):
    sources = {"base": (None, BASE), "leaf": (None, leaf(edits, merge))}
    return bc.resolve("leaf", {}, sources)


class Edits(unittest.TestCase):
    def test_identity_is_the_leaf_and_merge_recurses(self):
        d = resolved([], {"metadata": {"display_name": "Leaf"}})
        self.assertEqual((d["id"], d["recipe"], d["kind"]), ("leaf", "leaf", "local"))
        self.assertEqual(d["metadata"]["display_name"], "Leaf")
        self.assertEqual(len(d["metadata"]["options"]), 2)

    def test_merged_array_replaces(self):
        d = resolved([], {"metadata": {"options": [{"id": "c"}]}})
        self.assertEqual(d["metadata"]["options"], [{"id": "c"}])

    def test_add_record_with_anchors(self):
        d = resolved(
            [
                {"op": "add", "path": "/metadata/options", "value": {"id": "z"}},
                {
                    "op": "add",
                    "path": "/metadata/options",
                    "value": {"id": "first"},
                    "before": {"id": "a"},
                },
                {
                    "op": "add",
                    "path": "/metadata/options",
                    "value": {"id": "mid"},
                    "after": {"id": "a"},
                },
            ]
        )
        ids = [o["id"] for o in d["metadata"]["options"]]
        self.assertEqual(ids, ["first", "a", "mid", "b", "z"])

    def test_matched_member_edits(self):
        d = resolved(
            [
                {
                    "op": "add",
                    "path": "/metadata/options",
                    "match": {"id": "a"},
                    "at": "/name",
                    "value": "n",
                },
                {"op": "remove", "path": "/metadata/options", "match": {"id": "b"}},
                {
                    "op": "replace",
                    "path": "/launch/argv",
                    "match": {"id": "x"},
                    "at": "/flag",
                    "value": "-y",
                },
            ]
        )
        self.assertEqual(d["metadata"]["options"], [{"id": "a", "name": "n"}])
        self.assertEqual(d["launch"]["argv"][0]["flag"], "-y")

    def test_clear_remove_merge_members(self):
        d = resolved(
            [
                {"op": "clear", "path": "/metadata/options"},
                {"op": "remove", "path": "/launch/environment"},
                {"op": "merge", "path": "/metadata", "value": {"extra": 1}},
            ]
        )
        self.assertEqual(d["metadata"]["options"], [])
        self.assertNotIn("environment", d["launch"])
        self.assertEqual(d["metadata"]["extra"], 1)

    def test_parent_is_not_mutated(self):
        sources = {"base": (None, BASE)}
        before = json.dumps(BASE, sort_keys=True)
        sources["leaf"] = (None, leaf([{"op": "clear", "path": "/metadata/options"}]))
        bc.resolve("leaf", {}, sources)
        self.assertEqual(json.dumps(BASE, sort_keys=True), before)

    def test_bad_edits_are_named(self):
        for edit, words in (
            (
                {"op": "replace", "path": "/metadata/missing", "value": 1},
                "missing_target",
            ),
            (
                {"op": "remove", "path": "/metadata/options", "match": {"id": "q"}},
                "matches 0",
            ),
            ({"op": "add", "path": "/metadata/display_name", "value": "x"}, "absent"),
            ({"op": "remove", "path": "/kind", "match": {"id": "a"}}, "keyed record"),
        ):
            with self.assertRaises(bc.DefinitionError, msg=edit) as e:
                resolved([edit])
            self.assertIn(words, str(e.exception))

    def test_missing_parent_and_cycles(self):
        with self.assertRaises(bc.DefinitionError):
            bc.resolve("leaf", {}, {"leaf": (None, leaf([], parent="gone"))})
        loop = leaf([], parent="leaf")
        with self.assertRaises(bc.DefinitionError):
            bc.resolve("leaf", {}, {"leaf": (None, loop)})


if __name__ == "__main__":
    unittest.main()
