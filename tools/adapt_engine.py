#!/usr/bin/env python3
"""Bring an engine into lemond from its binary: probe it, write its backends.d definition, grade it.

A thin entry. The program is tools/adapt_engine.py in festeraeb/lemonade-engine-adapters; this file finds
that checkout and fills in this tree's lemond, lemonade and backend_conformance.py as defaults.

    python tools/adapt_engine.py --binary /path/to/engine-server --model /path/to/model \\
        [--adapters /path/to/lemonade-engine-adapters] [--helper-url http://host:port/v1 | --no-helper]

The adapters checkout is --adapters, else $LEMONADE_ENGINE_ADAPTERS, else a folder named
lemonade-engine-adapters beside this repository. Every other option is the adapter program's own
(run it with --help).
"""
import os
import runpy
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def main(argv):
    adapters = None
    rest = []
    it = iter(argv)
    for a in it:
        if a == "--adapters":
            adapters = next(it, None)
        elif a.startswith("--adapters="):
            adapters = a.split("=", 1)[1]
        else:
            rest.append(a)
    adapters = Path(adapters or os.environ.get("LEMONADE_ENGINE_ADAPTERS") or REPO.parent / "lemonade-engine-adapters")
    program = adapters / "tools" / "adapt_engine.py"
    if not program.is_file():
        sys.exit(f"adapt_engine: {program} not found; clone festeraeb/lemonade-engine-adapters and pass --adapters")
    defaults = {
        "--lemond": REPO / "build" / "lemond",
        "--lemonade": REPO / "build" / "lemonade",
        "--conformance": REPO / "tools" / "backend_conformance.py",
    }
    for flag, path in defaults.items():
        if not any(a == flag or a.startswith(flag + "=") for a in rest) and path.exists():
            rest += [flag, str(path)]
    sys.argv = [str(program)] + rest
    runpy.run_path(str(program), run_name="__main__")


if __name__ == "__main__":
    main(sys.argv[1:])
