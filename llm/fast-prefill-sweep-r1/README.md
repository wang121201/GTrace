# Finite native Prefill sweep front end (r4 experiment)

This directory adds C++ global-Effect programs for the independently captured
Qwen2.5-1.5B P64/D2 kernels. It preserves the existing cache implementation and
source family/semantic names. The whole-stream switch is
`--fast-prefill-sweep`; commands use `qwen_prefill_sweep_program_v1`.

Only four exact P64 variants are currently implemented. P256/P512 are admitted
as requested experiment lengths by the graph contract but their new native
programs are not yet qualified. Length admission does not imply a runnable
model. No shape-scaled traffic estimate or implicit old-template fallback is
allowed. Actual graph/source/ABI/carveout and runner-evidence gates remain
mandatory before replay.

Validation: local full runner compiles; the corresponding independently written
Python source programs and this C++ path agree on 16,640 Effect records across
8 first/last CTA schedule cases, with 9 invalid commands rejected. Fresh actual
P64 source binding checks exist for the four primary GEMMs. Complete helper
source admission and Linux runner equivalence have not yet run: source upload
requires the pending explicit user authorization requested after automatic
approval review rejected it.

The experiment's immutable code packages, source-derived Python programs,
per-case proof and report live in the enclosing
`work/gtsim-ada-r4-prefill-sweep-20260922-r1` experiment directory. See CURRENT.md
there for concrete source paths, hashes, outstanding gates and restart steps.
P32/P128 closed r4 baselines are reused; this new experiment launches no r2 jobs.
