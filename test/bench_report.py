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
"""Report the optimistic minimal tracing overhead from a bench trace.

Reads the newest events_pid*.json in the given directory and, for consecutive same-thread events,
computes the smallest (and median) positive gap in nanoseconds - i.e. the closest two timestamps
the tracer ever managed to record, which approximates the minimum per-event overhead. Also reports
the event count and whether begin/end are balanced (a correctness check the runner gates on).

Usage: bench_report.py <dir>
Prints, one per line: EVENTS <n>, BALANCED <yes|no>, MIN_CONSECUTIVE_NS <x>, MEDIAN_CONSECUTIVE_NS <y>.
"""
import sys, os, glob, json
from collections import defaultdict
from statistics import median

directory = sys.argv[1] if len(sys.argv) > 1 else "."
files = sorted(glob.glob(os.path.join(directory, "events_pid*.json")), key=os.path.getmtime)
if not files:
    print("EVENTS 0"); print("BALANCED no")
    print("MIN_CONSECUTIVE_NS -1"); print("MEDIAN_CONSECUTIVE_NS -1")
    sys.exit(1)

events = json.load(open(files[-1]))["traceEvents"]
be = [e for e in events if e.get("ph") in ("B", "E")]
b = sum(1 for e in be if e["ph"] == "B")
e = sum(1 for e in be if e["ph"] == "E")

# ts is microseconds with a .NNN nanosecond fraction -> recover integer nanoseconds.
by_tid = defaultdict(list)
for x in be:
    by_tid[x["tid"]].append(round(float(x["ts"]) * 1000))

deltas = []
for tid, ts in by_tid.items():
    ts.sort()
    deltas.extend(ts[i] - ts[i - 1] for i in range(1, len(ts)) if ts[i] - ts[i - 1] > 0)

print(f"EVENTS {len(be)}")
print(f"BALANCED {'yes' if b == e else 'no'}")
print(f"MIN_CONSECUTIVE_NS {min(deltas) if deltas else -1}")
print(f"MEDIAN_CONSECUTIVE_NS {int(median(deltas)) if deltas else -1}")
