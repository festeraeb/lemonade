# Backend conformance

`tools/backend_conformance.py` grades how much control `lemond` has over one backend. It reads the backend's definition from the same JSON files `lemond` loads (`src/cpp/resources/backends/` and `<config_dir>/backends.d/`) and checks a running `lemond` through its HTTP API. The definitions are the source of truth: the tool keeps no description of any engine of its own. It uses only the Python standard library.

- [Levels](#levels)
- [What it reads from a definition](#what-it-reads-from-a-definition)
- [Running it](#running-it)
- [Not gradable on this build](#not-gradable-on-this-build)

## Levels

| Level | Passes when |
|---|---|
| L1 lifecycle | A chat request loads the model and gets an answer, and unloading ends the engine process (pid from `GET /v1/health`) and frees the GPU memory it held |
| L2 eviction | With `max_loaded_models` at 1, loading a second model of the same type evicts the first and its engine exits. Needs `--other` |
| L3 device | Loading with the definition's device option puts the engine's memory only on that GPU, and the value appears after the option's flag in `launch_command`. Needs `--device` and GPU readings |
| L4 telemetry | `/metrics` carries engine samples labelled with the model, which `lemond` forwards for definitions that set `metadata.exposes_prometheus_metrics` |

The earned level is the highest level that passes together with every level below it. Each level reports one of:

| Result | Meaning |
|---|---|
| `pass` | The check ran and passed |
| `FAIL` | The check ran and failed; the detail says what was seen |
| `not tested` | An input was not given (`--other`, `--device`, or GPU readings) |
| `not gradable` | The definition or this `lemond` lacks what the check reads; the detail names it |

GPU memory is read with `all-smi`, falling back to `nvidia-smi`. With `--cpu-only`, or when neither tool answers, memory is checked only by the engine process exiting, L3 cannot pass, and the earned level stops at L2.

## What it reads from a definition

The tool resolves `extends`, `merge` and `edits` as described in [Backend definition v2](backend-definition-v2.md), then reads:

| The check needs | Definition field |
|---|---|
| The recipe the model must use | `recipe` |
| Proof that `lemond` runs this file | The definition hash, compared with `definition_hash` in `GET /v1/backends/schema?recipe=RECIPE` |
| Load timeout | `launch.readiness.timeout` |
| A request to answer | An `operations` record with `operation: chat_completion`, or `chat` in `metadata.supported_modes` |
| The device option to send in `POST /v1/load` | The `metadata.options` record with `id: device` (v1: `type_name: DEVICES`), by its `name` |
| That the option reaches the engine | A `launch` record that refers to that option; its `flag` is looked up in `launch_command` |
| Whether the engine's metrics are forwarded | `metadata.exposes_prometheus_metrics` |

When several definitions share a recipe, such as container variants selected by the backend option, the schema row names one of them and the other hashes are not compared.

`test/test_backend_conformance_definitions.py` checks that the resolved hashes of the built-in `llamacpp` and `hrx` definitions equal the pins compiled into `lemond`.

## Running it

```bash
# Unit tests
python3 -m unittest test/test_backend_conformance_definitions.py \
    test/test_backend_conformance_levels.py test/test_backend_conformance_gpu.py

# Built-in definition, two models of its recipe
python3 tools/backend_conformance.py --lemond http://127.0.0.1:13305 \
    --definition llamacpp --model MODEL --other MODEL2 --device CUDA1

# External definition
python3 tools/backend_conformance.py --lemond http://127.0.0.1:13305 \
    --backends-d ~/.config/lemonade/backends.d --definition research-llama \
    --model MODEL --other MODEL2 --device none --cpu-only
```

`--definition` takes a definition ID or the path to its file. `--device-gpu` names the GPU index a device value selects when its trailing number is not that index. `--json` prints the report as JSON. The exit code is 0 when the backend earns at least L1.

## Not gradable on this build

The tool notes, without grading, that `GET /v1/stats` carries no per-engine data. An engine's memory use per device, which an eviction policy could use to ask an engine to shrink rather than unload it, has no route through `lemond` today; `/metrics` forwards only what the engine's own `/metrics` exposes.
