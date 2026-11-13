#!/usr/bin/env python3
"""TEST FIXTURE: a stand-in prepare step for prepare_live.py. Prints a few lines, appends one line to
--count, writes <model>.prepared, and prints its argv so a test can see that nothing was run by a shell.
--fail exits 3 after printing; --sleep N waits N seconds first."""

import json, sys, time

a = sys.argv[1:]


def opt(name, default=None):
    return a[a.index(name) + 1] if name in a else default


if "--sleep" in a:
    time.sleep(float(opt("--sleep")))
print("fixture prepare: argv " + json.dumps(a), flush=True)
print("fixture prepare: converting " + str(opt("--model")), flush=True)
if "--count" in a:
    with open(opt("--count"), "a") as f:
        f.write("run\n")
if "--fail" in a:
    print("fixture prepare: unsupported tensor type q9_9 in layer 7", flush=True)
    print("fixture prepare failed on purpose", flush=True)
    sys.exit(3)
with open(opt("--model") + ".prepared", "w") as f:
    f.write("prepared\n")
print("fixture prepare: done", flush=True)
