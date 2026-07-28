# Contributing

Contributions to the simulator core and reference workloads are welcome.

Timing changes must be justified by a documented shared hardware profile or a
microbenchmark, not by per-workload latency overrides. Run the build, CTest
suite, `scripts/check_workload_profile.py`, and the relevant validation sweep
before proposing a change. Do not add traces, proprietary profiler exports, or
third-party code/data unless its redistribution terms are documented.
