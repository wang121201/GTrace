# H100 validation data

These are the H100 validation cycle targets used by the v0.1.0 public release.

- `data/h100_gemm.csv` has 8 persistent two-stage GEMM points, selected from
  the smaller M/N/K combinations (each dimension is at most 1024).
- `data/h100_fa3.csv` has 6 H100 attention-pipeline points, covering 8 and 16
  heads at sequence lengths 512, 1024, and 1536. These are selected from the
  smaller configurations, excluding the 2048-length cases.

Run `python3 scripts/run_validation.py --build-dir build` to produce per-case
simulated cycles, absolute percentage errors, MAPE, and Pearson correlation.
The command writes generated reports under `validation/results/`, which is not
versioned so that reports always identify the exact simulator revision used.
For machines with a short job limit, split a long sweep with `--start-case` and
`--max-cases`; each invocation is independently reported.
