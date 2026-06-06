# Tests

End-to-end tests for the background spill-to-disk feature (`LOP_SPILL_TO_DISK`).

```sh
./test/run_tests.sh
```

Linux/x64 only; needs `g++` and `python3`. The script prints `ALL TESTS PASSED` and exits 0 on
success.

The profiler is header-only, so each scenario just compiles the multi-threaded harness
`spill_test.cpp` with the relevant config overridden via `-D` (a tiny `LOP_BUFFER_SIZE` to force
buffer rotation cheaply, or spilling toggled on/off), runs it, and validates the resulting trace
with `validate.py`.

There is also `./test/run_bench.sh`, which builds `bench.cpp` and reports per-event tracing
overhead (wall-time delta of tracing on vs off) plus the smallest observed gap between two
consecutive events.

## What it does

| Scenario | What it checks |
|----------|----------------|
| 1. Forced spill (tiny buffer, 4 threads, ASan/UBSan) | Many filled buffers are spilled to disk and merged back at flush; **no event lost**, per-thread B/E balanced, no undefined behavior. |
| 2. ON vs OFF equivalence | Spilling on and off produce identical event counts. |
| 3. Flag-off build | With `LOP_SPILL_TO_DISK 0` there is no spiller and no temp file; behavior is unchanged. |
| 4. OOM relief valve | Under a tight `ulimit -v`, buffer allocations fail; the profiler boosts the spiller to reclaim RAM and retries **instead of crashing**, and still loses no events. |

`spill_test.cpp` emits a known number of `begin`/`end` pairs per thread plus a flow pair per
thread; `validate.py` picks the newest `events_pid*.json` and asserts the exact counts survived.

Note: scenario 4 depends on `ulimit -v` behavior and timing; if your environment can't enforce the
cap it may not trigger the relief valve. The other scenarios are deterministic.
