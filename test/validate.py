#!/usr/bin/env python3
# Copyright (c) 2025-2026 Krzysztof Badziak
#
# Licensed under the MIT License:
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
"""Validate a trace produced by spill_test.cpp.

Picks the newest events_pid*.json in the given directory and asserts that every emitted event
survived the disk/RAM merge: the expected number of worker begin/end events, the expected flow
events, and that every thread's B/E counts are balanced (nesting intact).

Usage: validate.py <dir> <expected_worker_pairs> <expected_flows>
Prints a one-line summary plus "TOTAL <n>" (for equivalence comparisons) and exits non-zero on
any mismatch.
"""
import sys, os, json, glob
from collections import Counter, defaultdict

if len(sys.argv) != 4:
    print("usage: validate.py <dir> <expected_worker_pairs> <expected_flows>")
    sys.exit(2)

directory = sys.argv[1]
expect_pairs = int(sys.argv[2])
expect_flows = int(sys.argv[3])

files = sorted(glob.glob(os.path.join(directory, "events_pid*.json")), key=os.path.getmtime)
if not files:
    print(f"  FAIL: no events_pid*.json in {directory}")
    sys.exit(1)

events = json.load(open(files[-1]))["traceEvents"]
phases = Counter(e.get("ph") for e in events)

names = {"w0", "w1", "w2", "w3"}
worker_b = sum(1 for e in events if e.get("ph") == "B" and e.get("name") in names)
worker_e = sum(1 for e in events if e.get("ph") == "E" and e.get("name") in names)

per_tid = defaultdict(Counter)
for e in events:
    if e.get("ph") in ("B", "E"):
        per_tid[e["tid"]][e["ph"]] += 1
unbalanced = {t: dict(c) for t, c in per_tid.items() if c["B"] != c["E"]}

ok = (worker_b == expect_pairs and worker_e == expect_pairs
      and phases.get("s", 0) == expect_flows and phases.get("f", 0) == expect_flows
      and not unbalanced)

print(f"  {os.path.basename(files[-1])}: total={len(events)} "
      f"worker B/E={worker_b}/{worker_e} (want {expect_pairs}) "
      f"flows s/f={phases.get('s',0)}/{phases.get('f',0)} (want {expect_flows}) "
      f"unbalanced_tids={unbalanced or 'none'}  -> {'PASS' if ok else 'FAIL'}")
print(f"TOTAL {len(events)}")
sys.exit(0 if ok else 1)
