# Tests

End-to-end tests for the background spill-to-disk feature (`LOP_SPILL_TO_DISK`).

```sh
./test/run_tests.sh
```

Linux/x64 only (uses the GCC inline-asm backend `src/profiler_asm.cpp`); needs `g++` and
`python3`. The script prints `ALL TESTS PASSED` and exits 0 on success.

## What it does

`LOP_BUFFER_SIZE` and the feature flags are compile-time constants, so each scenario compiles the
profiler from a patched copy of the sources (a tiny buffer to force buffer rotation cheaply, or
spilling toggled on/off), runs the multi-threaded harness `spill_test.cpp`, and validates the
resulting trace with `validate.py`.

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
