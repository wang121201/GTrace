# Finite native Prefill sweep front end (r4 experiment)

This directory adds C++ global-Effect programs for the independently captured
Qwen2.5-1.5B P64/D2 kernels. It preserves the existing cache implementation and
source family/semantic names. The whole-stream switch is
`--fast-prefill-sweep`; commands use `qwen_prefill_sweep_program_v1`.

The four exact P64 variants have completed whole-inference r4 replay. New QKV,
Gate, O and Down variants for P256/P512 share this command dispatcher and preserve
their separately reviewed source formulas. The finite contract includes shape,
code and dispatch geometry: matching a code alone does not admit another shape.
Attention completion, actual interface checks and final graph admission remain
separate gates. No shape-scaled traffic estimate or implicit template
fallback is allowed. The cache implementation is unchanged.

Validation: local full runner compiles; the corresponding independently written
Python source programs and this C++ path agree on 16,640 Effect records across
8 first/last CTA schedule cases, with 9 invalid commands rejected. Complete P64
helper admission, Linux runner validation and all-launch resource matching have
also passed. P256/P512 Gate, O and Down have fresh actual ABI/full-static and
Linux C++ source-Effect evidence. QKV also has both current-case Linux checks,
including its two trailing-predicate-false drain iterations per CTA. This does
not replace final whole-graph checks.
Python/C++ agreement concerns the modeled source projection, not measured GPU
issue order, hardware poll counts or acceptance of NCU traffic error.

QKV uses a complete ascending serial split CTA schedule. Part0 publishes before
dependent parts read its semaphore; an exact source-driven initialization
witness is required. The chosen schedule emits one successful poll per
dependent warp. Its EL semaphore load follows the existing normal-priority
approximation, recorded separately by the producer. Internal shared-memory
barrier timing and real GPU warp issue order are outside this functional trace.

The experiment's immutable code packages, source-derived Python programs,
per-case proof and report live in the enclosing
`work/gtsim-ada-r4-prefill-sweep-20260922-r1` experiment directory. See CURRENT.md
there for concrete source paths, hashes, outstanding gates and restart steps.
P32/P128 closed r4 baselines are reused; this new experiment launches no r2 jobs.
