# Rerun saved readers

These files are separate from the frozen controller, original admission, plan, binary and input. They do not launch, interrupt or poll the simulator. Root owns the one active `run-r1`.

One progress snapshot, from the repository root:

```sh
python3 -B work/ada-cosim-alignment-20260922-r1/gddr-full-p2-rerun-r1/progress_report.py
```

The output is `outputs/ada-gddr-p2-rerun-r1/{index.html,progress.json}`. It shows its snapshot timestamp and actual controller status. There is no automatic updater; reload alone does not fetch new native progress. This reader never opens the new `result.json`. It checks the published completed journal prefix against the old CLOSED run and only displays phases with both epoch boundaries. No Full extrapolation, ETA or CPU speed inference is made.

Only after the controller's successful CLOSED receipt, run the complete saved comparison with a **fresh** output directory:

```sh
python3 -B work/ada-cosim-alignment-20260922-r1/gddr-full-p2-rerun-r1/report_compare.py \
  --output outputs/ada-gddr-p2-rerun-r1/result-r1
```

The wrapper locks the old comparator SHA `bde92de92251ed87c5a1706285d9b27abdbb2afc25a4ff5bfc7ce76427418c9f` and new host manifest SHA `b52bd3f0514b1ca50ab69ee3462d0086d17adf891e55fe13f742afae23ba8a3c`. The original comparator cannot be used directly: its receipt gate requires 5400 seconds, its aggregate labels that budget, and it requires a fresh plan `preparation.json` that this reuse run does not have.

The wrapper checks the actual 7200/7140/60/3600 budget and exact host override/admission/controller pins first. A copy of the receipt, changing only the old gate's fixed `max_seconds` assertion, reuses every original lifecycle check; the saved receipt is never changed. All original full work, physical requests/completions, async/HBF, dirty and writer ledgers, config delta, source identity and NCU checks remain in the frozen comparator. RUNNING or FAIL is rejected before opening any new result. The actual reported budget is 7200 seconds.

The independent source preflight measured 0.704641875 seconds, which is only one preparation component. Whole external preparation is unmeasured and stays null. No old 1.597-second preparation or historical build is added. The 3600-second performance target is unchanged. The source checks contain no execution or new final-result read.

Both readers preserve the hardware scope: PF/Full are direct cold prefixes, while Decode values are cross-run cumulative-prefix differences with observed variation, not direct same-run phase counters. Existing cache/R4, compute and DRAM timing limits are not upgraded by the rerun.

`reader-checks.json` records source AST, synthetic budget/cleanup rejection and saved-old-receipt checks. It is not a completion receipt. `report_compare.py` was not executed against the running model.
