#!/usr/bin/env python3
"""Grade how much control lemond has over a backend, reading the backend's JSON definition.

The definition is the source of truth. The recipe, readiness probe, device option and metrics
declaration are read from the definition file (src/cpp/resources/backends/*.json, or
<config_dir>/backends.d/*.json for an external one), resolved through `extends`, `merge` and
`edits` as described in docs/dev/backend-definition-v2.md. The checks then run against a running
lemond through its normal HTTP API. The resolved definition's hash is compared with the
definition_hash lemond reports in GET /v1/backends/schema, so a result names the file it graded.
Levels are tried in order; the backend earns the highest level whose checks pass together with
every level below it.

    L1 lifecycle   loading starts the engine, it answers a chat request, and unloading ends the
                   engine process and frees the GPU memory it held
    L2 eviction    loading a second model of the same type evicts this one and frees its memory
    L3 device      the definition's device option is honoured: memory appears only on that GPU
    L4 telemetry   the engine's own metrics reach the client through lemond's /metrics

    python tools/backend_conformance.py --lemond http://127.0.0.1:13305 --definition llamacpp \\
        --model <model> [--other <second model>] [--device CUDA1 [--device-gpu 1]] [--cpu-only]

A level is "not tested" when an input it needs was not given, and "not gradable" when the
definition or this build of lemond lacks what the check reads; the detail says what is missing.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

PROMPT = "In one sentence, what is a lighthouse for?"
REPO = Path(__file__).resolve().parent.parent
BUILTIN_DIR = REPO / "src" / "cpp" / "resources" / "backends"
IDENTITY_FIELDS = {
    "/metadata/options": "id",
    "/metadata/support": "id",
    "/install/cases": "id",
    "/install/native/cases": "id",
    "/launch/execution/cases": "id",
    "/launch/mounts": "id",
    "/launch/argv": "id",
    "/launch/custom_args/defaults": "id",
    "/operations": "id",
    "/launch/ports": "name",
    "/launch/environment/set": "name",
}
ENVELOPE_KEYS = ("schema_version", "id", "recipe", "extends", "merge", "edits")
PASS, FAIL, NOT_TESTED, NOT_GRADABLE = "pass", "FAIL", "not tested", "not gradable"


class DefinitionError(Exception):
    pass


def read_sources(directory: Path) -> dict[str, tuple[Path, dict]]:
    sources = {}
    if directory.is_dir():
        for path in sorted(directory.glob("*.json")):
            data = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(data, dict) and isinstance(data.get("id"), str):
                sources[data["id"]] = (path, data)
    return sources


def merge_into(base, over):
    if not isinstance(base, dict) or not isinstance(over, dict):
        return copy.deepcopy(over)
    result = dict(base)
    for key, value in over.items():
        result[key] = (
            merge_into(base.get(key), value) if key in base else copy.deepcopy(value)
        )
    return result


def split_pointer(pointer: str) -> list[str]:
    if not pointer.startswith("/"):
        raise DefinitionError(f"pointer {pointer!r} does not start with /")
    return [t.replace("~1", "/").replace("~0", "~") for t in pointer[1:].split("/")]


def walk(root, tokens: list[str], pointer: str):
    node = root
    for token in tokens:
        if not isinstance(node, dict) or token not in node:
            raise DefinitionError(f"missing_target at {pointer}")
        node = node[token]
    return node


def find_record(records, field: str, value, pointer: str) -> int:
    hits = [
        i
        for i, r in enumerate(records)
        if isinstance(r, dict) and r.get(field) == value
    ]
    if len(hits) != 1:
        raise DefinitionError(
            f"{field}={value!r} matches {len(hits)} records at {pointer}"
        )
    return hits[0]


def apply_member(parent, key, edit, pointer: str):
    op, value = edit["op"], copy.deepcopy(edit.get("value"))
    if op == "add":
        if isinstance(parent, dict) and key not in parent:
            parent[key] = value
            return
        raise DefinitionError(f"add needs an absent member at {pointer}")
    if not isinstance(parent, dict) or key not in parent:
        raise DefinitionError(f"missing_target at {pointer}")
    if op == "replace":
        parent[key] = value
    elif op == "remove":
        del parent[key]
    elif op == "clear" and isinstance(parent[key], (dict, list)):
        parent[key].clear()
    elif op == "merge" and isinstance(parent[key], dict) and isinstance(value, dict):
        parent[key] = merge_into(parent[key], value)
    else:
        raise DefinitionError(f"{op} does not apply at {pointer}")


def apply_edit(payload: dict, edit: dict) -> None:
    path = edit["path"]
    tokens = split_pointer(path)
    match = edit.get("match")
    if match is None:
        field = IDENTITY_FIELDS.get(path)
        target = walk(payload, tokens[:-1], path).get(tokens[-1]) if field else None
        if edit["op"] == "add" and isinstance(target, list):
            index = len(target)
            for anchor, offset in (("before", 0), ("after", 1)):
                if anchor in edit:
                    afield, avalue = next(iter(edit[anchor].items()))
                    index = find_record(target, afield, avalue, path) + offset
            target.insert(index, copy.deepcopy(edit["value"]))
            return
        apply_member(walk(payload, tokens[:-1], path), tokens[-1], edit, path)
        return
    records = walk(payload, tokens, path)
    field, value = next(iter(match.items()))
    if path not in IDENTITY_FIELDS or not isinstance(records, list):
        raise DefinitionError(f"{path} is not a keyed record array")
    index = find_record(records, field, value, path)
    if "at" in edit:
        sub = split_pointer(edit["at"])
        apply_member(walk(records[index], sub[:-1], path), sub[-1], edit, path)
    elif edit["op"] == "remove":
        del records[index]
    elif edit["op"] == "replace":
        records[index] = copy.deepcopy(edit["value"])
    elif edit["op"] == "merge":
        records[index] = merge_into(records[index], edit["value"])
    else:
        raise DefinitionError(
            f"{edit['op']} does not apply to a whole record at {path}"
        )


def resolve(
    key: str, builtin: dict, external: dict, namespace: str = "external", depth: int = 0
):
    """The materialized declaration for one definition, as lemond's resolver builds it."""
    table = builtin if namespace == "builtin" else external
    if key not in table:
        raise DefinitionError(f"no {namespace} definition with id {key!r}")
    if depth >= 8:
        raise DefinitionError("extends chain is longer than 8 documents")
    envelope = table[key][1]
    if envelope.get("schema_version") != 2:
        return copy.deepcopy(envelope)
    payload = {}
    parent = envelope.get("extends")
    if parent:
        parent_ns = "builtin" if "builtin" in parent else "external"
        declaration = resolve(
            parent[parent_ns], builtin, external, parent_ns, depth + 1
        )
        if declaration.get("schema_version") != 2:
            raise DefinitionError(
                f"parent {parent[parent_ns]!r} is not a v2 definition"
            )
        payload = {k: v for k, v in declaration.items() if k not in ENVELOPE_KEYS}
    payload = merge_into(payload, envelope.get("merge", {}))
    for edit in envelope.get("edits", []):
        apply_edit(payload, edit)
    return {
        "schema_version": 2,
        "id": envelope["id"],
        "recipe": envelope["recipe"],
        **payload,
    }


def canonical(value) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode()


def definition_hash(declaration: dict) -> str:
    if declaration.get("schema_version") == 2:
        data = b"lemonade.backend-definition.v2\n" + canonical(declaration)
    else:
        data = canonical(declaration)
    return "sha256:" + hashlib.sha256(data).hexdigest()


def option_refs(node, names: set[str]) -> bool:
    if isinstance(node, dict):
        if node.get("option") in names:
            return True
        return any(option_refs(v, names) for v in node.values())
    if isinstance(node, list):
        return any(option_refs(v, names) for v in node)
    return False


def facts(declaration: dict) -> dict:
    """What the checks need to know about an engine, taken from its definition."""
    meta = declaration.get("metadata", {})
    launch = declaration.get("launch", {})
    device = next(
        (
            o
            for o in meta.get("options", [])
            if o.get("id") == "device" or o.get("type_name") == "DEVICES"
        ),
        None,
    )
    names = {device.get("id"), device.get("name")} - {None} if device else set()
    argv = [r for r in launch.get("argv", []) if names and option_refs(r, names)]
    operations = [o.get("operation") for o in declaration.get("operations", [])]
    return {
        "id": declaration.get("id"),
        "recipe": declaration.get("recipe"),
        "schema_version": declaration.get("schema_version"),
        "display_name": meta.get("display_name"),
        "readiness": launch.get("readiness"),
        "device_option": device.get("name") if device else None,
        "device_bound": bool(names) and option_refs(launch, names),
        "device_flag": argv[0].get("flag") if argv else None,
        "metrics": bool(meta.get("exposes_prometheus_metrics")),
        "chat": "chat_completion" in operations
        or "chat" in meta.get("supported_modes", []),
        "hash": definition_hash(declaration),
    }


def call(base: str, path: str, body: dict | None = None, timeout: int = 1900):
    req = urllib.request.Request(
        base + path,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"content-type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read()
    return (
        raw.decode("utf-8", "replace")
        if path == "/metrics"
        else json.loads(raw or b"{}")
    )


def same_model(name: str, wanted: str) -> bool:
    return name == wanted or name.endswith("." + wanted)


def loaded(base: str) -> list[dict]:
    return call(base, "/api/v1/health", timeout=10).get("all_models_loaded", [])


def entry(base: str, model: str) -> dict | None:
    return next(
        (m for m in loaded(base) if same_model(m.get("model_name", ""), model)), None
    )


def unload(base: str, model: str) -> None:
    call(base, "/api/v1/unload", {"model_name": model}, timeout=120)


def _run(cmd: list[str]) -> str:
    return subprocess.run(
        cmd, capture_output=True, text=True, timeout=30, check=True
    ).stdout


def gpu_mib() -> dict[int, int]:
    """GPU index -> MiB used, from all-smi or nvidia-smi. Empty when neither answers."""
    try:
        data = json.loads(_run([os.environ.get("ALL_SMI", "all-smi"), "snapshot"]))
        gpus = [g for g in data.get("gpus", []) if g.get("device_type", "GPU") == "GPU"]
        return {i: g.get("used_memory", 0) // 1048576 for i, g in enumerate(gpus)}
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    try:
        out = _run(
            [
                "nvidia-smi",
                "--query-gpu=index,memory.used",
                "--format=csv,noheader,nounits",
            ]
        )
    except (OSError, subprocess.SubprocessError):
        return {}
    result = {}
    for line in out.splitlines():
        if re.match(r"^\s*\d+,\s*\d+\s*$", line):
            idx, mem = line.split(",")
            result[int(idx)] = int(mem)
    return result


def gpu_held(idle: dict[int, int], slack_mib: int = 300) -> dict[int, int]:
    return {
        i: m - idle.get(i, 0)
        for i, m in gpu_mib().items()
        if m - idle.get(i, 0) > slack_mib
    }


def wait_freed(idle: dict[int, int], slack_mib: int = 300, secs: int = 20) -> bool:
    for _ in range(secs):
        now = gpu_mib()
        if now and all(now.get(i, 0) <= used + slack_mib for i, used in idle.items()):
            return True
        time.sleep(1)
    return False


def pid_alive(pid: int) -> bool:
    return Path(f"/proc/{pid}").exists()


def rss_mib(pid: int) -> int | None:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) // 1024
    except OSError:
        return None
    return None


def wait_gone(pid: int, secs: int = 20) -> bool:
    for _ in range(secs * 2):
        if not pid_alive(pid):
            return True
        time.sleep(0.5)
    return False


def local_pid(base: str, info: dict | None) -> int | None:
    host = urllib.parse.urlparse(base).hostname
    pid = (info or {}).get("pid")
    if host in ("127.0.0.1", "localhost", "::1") and isinstance(pid, int) and pid > 0:
        return pid
    return None


def chat(base: str, model: str, timeout: int) -> str:
    body = {
        "model": model,
        "max_tokens": 60,
        "messages": [{"role": "user", "content": PROMPT}],
    }
    r = call(base, "/api/v1/chat/completions", body, timeout=timeout)
    message = (r.get("choices") or [{}])[0].get("message", {})
    return message.get("content") or message.get("reasoning_content") or ""


def catalog_row(base: str, recipe: str) -> dict | None:
    try:
        reply = call(
            base,
            "/api/v1/backends/schema?recipe=" + urllib.parse.quote(recipe),
            timeout=20,
        )
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise
    rows = reply.get("recipes", [])
    return next((r for r in rows if r.get("recipe") == recipe), None)


def metrics_verdict(text: str, model: str) -> tuple[bool, str]:
    """(passed, what was seen) for engine metrics that lemond relabelled for this model."""
    families = set()
    for line in text.splitlines():
        if line.startswith("lemonade_llamacpp_") and re.search(
            r'model_name="([^"]*\.)?' + re.escape(model) + '"', line
        ):
            families.add(re.split(r"[{\s]", line, maxsplit=1)[0])
    if not families:
        return False, f"/metrics has no engine samples labelled model_name={model!r}"
    return True, f"/metrics carries {len(families)} engine metric families for {model}"


def argv_value(argv: list, flag: str) -> str | None:
    for i, arg in enumerate(argv[:-1]):
        if arg == flag:
            return argv[i + 1]
    return None


def load_timeout(readiness: dict | None) -> int:
    value = (readiness or {}).get("timeout")
    if isinstance(value, dict):
        value = value.get("floor_seconds")
    return (value if isinstance(value, int) else 600) + 120


def run(
    base: str,
    info: dict,
    model: str,
    other: str | None,
    device: str | None,
    device_gpu: int | None,
    cpu_only: bool,
) -> dict:
    report = {
        "model": model,
        "definition": info,
        "levels": {},
        "earned": 0,
        "notes": [],
    }
    timeout = load_timeout(info["readiness"])

    def result(level: int, status: str, detail: str) -> None:
        report["levels"][f"L{level}"] = {"status": status, "detail": detail}
        if status == PASS and report["earned"] == level - 1:
            report["earned"] = level

    row = catalog_row(base, info["recipe"])
    if row is None:
        result(
            1,
            FAIL,
            f"lemond has no recipe {info['recipe']!r}: the definition is not installed",
        )
        return report
    if row.get("definition_hash") == info["hash"]:
        report["catalog"] = f"lemond runs this definition ({info['hash'][:19]}...)"
    elif row.get("definition_id") not in (None, info["id"]):
        report["catalog"] = (
            f"recipe {info['recipe']!r} is listed under definition "
            f"{row['definition_id']!r}; this definition shares the recipe, so its hash "
            "cannot be compared"
        )
    else:
        report["catalog"] = (
            f"lemond's definition_hash {str(row.get('definition_hash'))[:19]}... differs from the "
            f"file ({info['hash'][:19]}...): lemond reads definitions at startup"
        )
    models = call(base, "/api/v1/models?show_all=true", timeout=20).get("data", [])
    listed = next((m for m in models if same_model(m.get("id", ""), model)), None)
    if listed and listed.get("recipe") not in (None, info["recipe"]):
        result(
            1,
            FAIL,
            f"{model} uses recipe {listed.get('recipe')!r}, not {info['recipe']!r}",
        )
        return report
    if not info["chat"]:
        result(
            1, NOT_GRADABLE, "the definition declares no chat operation to answer with"
        )
        return report

    if entry(base, model):
        unload(base, model)
        time.sleep(3)
    idle = {} if cpu_only else gpu_mib()
    gpu = bool(idle)
    if not gpu:
        report["notes"].append(
            "GPU memory not checked (--cpu-only or no all-smi/nvidia-smi): "
            "unload is checked by the engine process exiting"
        )

    t0 = time.time()
    try:
        answer = chat(base, model, timeout)
    except Exception as e:  # noqa: BLE001 - any failure here means no lifecycle control
        result(1, FAIL, f"load/chat failed: {e}")
        report["notes"].append(
            "falls back to L0: run the engine without lemond's control"
        )
        return report
    secs = round(time.time() - t0, 1)
    pid = local_pid(base, entry(base, model))
    rss = rss_mib(pid) if pid else None
    held = gpu_held(idle) if gpu else {}
    unload(base, model)
    gone = wait_gone(pid) if pid else True
    freed = wait_freed(idle) if gpu else True
    ok = bool(answer.strip()) and entry(base, model) is None and gone and freed
    seen = [f"answered in {secs}s ({answer.strip()[:60]!r})"]
    if pid:
        seen.append(
            f"engine pid {pid} RSS {rss} MiB {'exited' if gone else 'still running'}"
        )
    else:
        seen.append("engine pid not visible from here")
    if gpu:
        seen.append(
            f"GPU held {held or 'nothing'}, after unload {'freed' if freed else 'NOT freed'}"
        )
    result(1, PASS if ok else FAIL, "; ".join(seen))
    if not ok:
        report["notes"].append(
            "falls back to L0: run the engine without lemond's control"
        )
        return report

    if other:
        chat(base, model, timeout)
        first = local_pid(base, entry(base, model))
        chat(base, other, timeout)
        evicted = entry(base, model) is None
        gone = wait_gone(first) if first else True
        unload(base, other)
        freed = wait_freed(idle) if gpu else True
        detail = f"loading {other} {'evicted' if evicted else 'did NOT evict'} {model}"
        if first:
            detail += (
                f"; its engine pid {first} {'exited' if gone else 'still running'}"
            )
        if gpu:
            detail += f"; GPU memory {'freed' if freed else 'NOT freed'} after both"
        if not evicted:
            detail += " (needs max_loaded_models 1 and two models of the same type)"
        result(2, PASS if evicted and gone and freed else FAIL, detail)
    else:
        result(2, NOT_TESTED, "pass --other <a second model of the same type>")

    if not info["device_option"]:
        result(3, NOT_GRADABLE, "the definition declares no device option")
    elif not info["device_bound"]:
        result(
            3,
            NOT_GRADABLE,
            f"option {info['device_option']} is not used by the launch section",
        )
    elif device is None:
        result(3, NOT_TESTED, f"pass --device <a {info['device_option']} value>")
    else:
        want = device_gpu
        if want is None and re.search(r"(\d+)$", device):
            want = int(re.search(r"(\d+)$", device).group(1))
        try:
            body = {"model_name": model, info["device_option"]: device}
            call(base, "/api/v1/load", body, timeout)
            argv = (entry(base, model) or {}).get("launch_command")
            flag = info["device_flag"]
            seen = f"{info['device_option']}={device}"
            arrived = True
            if argv and flag:
                arrived = argv_value(argv, flag) == device
                seen += f"; engine argv {'has' if arrived else 'lacks'} {flag} {device}"
            if not gpu:
                result(
                    3,
                    NOT_TESTED,
                    seen + "; GPU placement not read without GPU readings",
                )
            else:
                used = gpu_held(idle)
                on_it = arrived and want in used and all(i == want for i in used)
                seen += f"; asked for GPU {want}, memory on {sorted(used) or 'no GPU'}"
                result(3, PASS if on_it else FAIL, seen)
        except Exception as e:  # noqa: BLE001
            result(3, FAIL, f"load with {info['device_option']}={device} failed: {e}")
        finally:
            unload(base, model)
            if gpu:
                wait_freed(idle)

    try:
        chat(base, model, timeout)
        stats = call(base, "/api/v1/stats", timeout=20)
        if not isinstance(stats, dict) or "engine_telemetry" not in stats:
            report["notes"].append(
                "engine memory report per device: not gradable on this build: "
                "/api/v1/stats has no engine_telemetry"
            )
        if not info["metrics"]:
            result(
                4,
                NOT_GRADABLE,
                "the definition does not set metadata.exposes_prometheus_metrics, "
                "and lemond reads engine telemetry from no other source",
            )
        else:
            ok, why = metrics_verdict(call(base, "/metrics", timeout=20), model)
            result(4, PASS if ok else FAIL, why)
    except Exception as e:  # noqa: BLE001
        result(4, FAIL, f"telemetry read failed: {e}")
    finally:
        unload(base, model)
    return report


def find_definition(ref: str, builtin: dict, external: dict) -> tuple[str, str]:
    path = Path(ref)
    if path.suffix == ".json" and path.is_file():
        data = json.loads(path.read_text(encoding="utf-8"))
        for namespace, table in (("builtin", builtin), ("external", external)):
            if (
                data.get("id") in table
                and table[data["id"]][0].resolve() == path.resolve()
            ):
                return data["id"], namespace
        external[data["id"]] = (path, data)
        return data["id"], "external"
    if ref in external:
        return ref, "external"
    if ref in builtin:
        return ref, "builtin"
    raise DefinitionError(
        f"no definition {ref!r} in {BUILTIN_DIR} or the backends.d folders given"
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--lemond", default="http://127.0.0.1:13305")
    ap.add_argument(
        "--definition", required=True, help="definition id or path to its .json file"
    )
    ap.add_argument(
        "--backends-d",
        action="append",
        default=[],
        type=Path,
        help="folder of external definitions (lemond's <config_dir>/backends.d)",
    )
    ap.add_argument("--builtin-dir", default=BUILTIN_DIR, type=Path)
    ap.add_argument("--model", required=True)
    ap.add_argument("--other", default=None)
    ap.add_argument(
        "--device", default=None, help="value for the definition's device option"
    )
    ap.add_argument(
        "--device-gpu",
        type=int,
        default=None,
        help="GPU index the device value selects (default: its trailing number)",
    )
    ap.add_argument("--cpu-only", action="store_true", help="do not read GPU memory")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    builtin = read_sources(a.builtin_dir)
    external = {}
    for folder in a.backends_d:
        external.update(read_sources(folder))
    try:
        key, namespace = find_definition(a.definition, builtin, external)
        info = facts(resolve(key, builtin, external, namespace))
    except (DefinitionError, KeyError, ValueError) as e:
        print(f"definition could not be read: {e}", file=sys.stderr)
        return 2
    report = run(
        a.lemond.rstrip("/"), info, a.model, a.other, a.device, a.device_gpu, a.cpu_only
    )
    if a.json:
        print(json.dumps(report, indent=2))
        return 0 if report["earned"] >= 1 else 1
    readiness = info["readiness"] or {}
    print(
        f"definition {info['id']} (schema {info['schema_version']}, recipe {info['recipe']})"
    )
    print(f"  readiness: GET {readiness.get('path')} within {readiness.get('timeout')}")
    print(
        f"  device option: {info['device_option'] or 'none'}"
        f"{' (used in launch)' if info['device_bound'] else ''}"
    )
    print(f"  engine metrics: {'declared' if info['metrics'] else 'not declared'}")
    if "catalog" in report:
        print(f"  {report['catalog']}")
    for level, r in report["levels"].items():
        print(f"{level}: {r['status']}  {r['detail']}")
    print(
        f"earned level: L{report['earned']}"
        + "".join(f"\n  {n}" for n in report["notes"])
    )
    return 0 if report["earned"] >= 1 else 1


if __name__ == "__main__":
    sys.exit(main())
