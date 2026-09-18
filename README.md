# Native TileGen trace/cosimulation source baseline

This independent repository imports the effective C native engine from the
`tilegen-hbf-traceoff-native-r1` compile plan. It contains no B8 implementation.
The source keeps the original logical `work/` hierarchy under `source/`.
Compiler VFS overlay substitutions are materialized at those logical paths;
only absolute `#include` paths are rewritten to relative paths. No simulation
semantics are changed by this import.

`provenance/source-map.json` records every logical/effective origin, original
and imported SHA-256, and each include rewrite. All 22 translation units were
scanned with the original compiler flags. Only their 171 non-system build
dependencies are imported, not historical run data or collected traces.

Build with `python3 build.py --output ../build-baseline --jobs 2`.
Use `--deps-only` in a separate fresh output directory to verify that all
non-system includes resolve within this repository. `--native` chooses the
platform CPU flag; `--thin-lto` is optional. The build requires a C++20 compiler,
the platform C++ runtime and zlib. It does not run a GPU workload or simulation.

The engine's runtime input manifests, captured native programs and machine
configurations remain separate, source-qualified inputs. Importing compilation
dependencies does not claim full-model accuracy, timing calibration, or a newly
validated input set. New trace and direct-memory features are developed on the
separate integration branch after the import baseline.
