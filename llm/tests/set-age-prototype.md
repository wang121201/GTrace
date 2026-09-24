# Set-local dirty-age prototype

Status: **local component prototype only**. No remote upload, full-model replay, GPU/NCU collection, accuracy claim, or commit has been performed for this candidate. The six global-age experiments remain the first acceptance gate.

## Behavior

`DiagnosticOptions::dirty_age_clock` defaults to `GLOBAL_FORWARDED_LINE`; explicit `SET_FORWARDED_LINE` changes only the dirty-age clock and its queue. The runner selects it with `TILEGEN_L2_DIRTY_AGE_CLOCK=global|set`. The existing `TILEGEN_L2_DIRTY_AGE_ACCESSES` remains an unsigned uint64 budget; configuration records its clock and units. The whole-stream caller checks both using `--expected-dirty-age-clock set --expected-dirty-age-accesses 3125` (an example candidate budget, not a recommendation supported by results). Explicit set mode requires an explicit expected budget, so it cannot silently inherit the old global 64M default. Age 0 disables expiry and queues in both modes.

For each L1-forwarded 128B request, the original global access counter still advances. In set mode, one counter for the unchanged `L2GroupedLru.group(original_byte_VA)` advances; only that group's queue can expire. Cold groups do not age. Every store refreshes the whole tag timer before same-tick expiry; 32B dirty sectors and per-byte masks remain unchanged. Capacity eviction wins before age service. Capacity and drain remove the appropriate queue entry. Explicit drain advances neither clock and preserves known bytes/tags/LRU.

The original global last-store histograms retain their old names and global units. New `selected_*_dirty_age_histogram` fields use `min_selected_ticks`/`max_selected_ticks`; `selected_clock` and `age_budget_unit` make the selected unit explicit. Full local observations include per-group ticks, tick histogram/sum, age-writeback bytes and dirty-line counts. The independent ledger checks queue uniqueness, iterator membership, group identity, clock ordering, expiry, dirty masks, and sum(group ticks) = global forwarded accesses. No semantic, phase, model or NCU-dependent rule exists.

Sector32/no-store-RFO, masked32B writeback, source order, L1, tag capacity, software group mapping, LRU and EF remain unchanged. Native trace records still do not carry byte masks; the existing functional observer/companion ledger retains them.

## Validation

Reproduce all bounded CPU checks:

```sh
python3 llm/tests/run_set_age_tests.py --output build/FRESH-set-age-validation --cxx clang++
```

The script builds with AddressSanitizer and UndefinedBehaviorSanitizer, reruns the original 282,160 global checks and 5 actual-main cases, and runs:

- Set-local directed cases (including real L1-hit requests that advance neither age clock): cross-group isolation, exact threshold, same-tick store refresh, whole-line timer with distinct partial sectors, capacity-before-age queue deletion, redirty, retained partial knowledge, no-RFO, end-drain clocks/ledger, and age0 exact global/set output equality.
- Seeded 2,500-access read/store/atomic sequence across two conflicting groups with mixed L1 forwarding and EF hints. Checks periodic independent dirty/owner ledgers, equal typed read and L1/tag counters, and exact ordered read projection across global/set/off after removing mixed-output request numbering. Final drains close the ledger.
- Eleven actual-runner cases: cross-128B API ranges with natural phase/owner ledger closure, and ten configuration cases: default/explicit global equality, set, set age0, malformed clocks and budgets. Whole-stream expected clock/budget mismatches and uint64 errors are rejected.
- Frozen sector32/global and age0 A/B against core revision `17f03b070ecb86fa034597a6a7ac972ff319bef7`: exact complete snapshots, original global-age observations and ordered mixed read/write records. The test runner exports and namespace-wraps this header into its build directory (with an alias for the unchanged `EfHitThrottle` type); no second core fixture is committed.
- Original four whole-stream source/boundary/drain tests.

The runner verifies actual compilation dependency hashes before/after building and testing. The receipt records the frozen original header SHA, exact source/binary hashes, sanitizer choices through the nested global receipt, CPU minutes and wall minutes. Component timings include sanitizer costs and are **not** an estimate of whole-model speed. Existing full-model source identity and numerical read invariance still need to be checked before accepting this candidate.

## Cost and limitations

At 20,480 groups, the tested platform reports `sizeof(std::list<CacheKey>) = 24`: group queue heads + group clocks + per-group age-WB byte totals require **819,200 B (800 KiB)** in vector payload. This is 160 KiB above the initial 640 KiB design estimate because the implementation also records per-group age writeback bytes. Each resident tag adds one 8B selected last-store field; overall layout/allocator overhead is separate. A dirty tag keeps one queue node and iterator, not a second tag table. Global mode does not allocate the per-group vectors but does retain the selected tick field and added diagnostics.

Per forwarded request, set mode adds a software group computation/counter update and group-local queue inspection; WB cost remains proportional to evicted dirty sectors. Snapshot verification scans group heads and resident tags and serializes the new local arrays. Full-run CPU/RSS overhead has not been measured. Safe-summary/report export should use aggregates if repeated 20,480-element arrays become too large.

This models accesses to a software group, not elapsed time or proven NVIDIA hardware writeback behavior. It depends on the retained surrogate address mapping; passing P32/P128 would require independent length/model/layout validation. Do not convert global budgets into set budgets without explicitly labeling the uniform-load normalization assumption.
