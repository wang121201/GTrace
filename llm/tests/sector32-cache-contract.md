# LLM sector32 data policy and dirty ownership

This is a functional traffic policy change in the new sector32 worktree. It does not alter the native source graph, r4 L1 mapping/replacement/capacity, global-VA L2 tags, 40 MiB grouped geometry, EF h288 interpretation, or the last-store age algorithm. The paired experiment still explicitly sets `TILEGEN_L2_DIRTY_AGE_ACCESSES=64000000`; the clock counts forwarded L2 line accesses, not time. No GPU execution or full-model accuracy claim belongs to this component delivery.

## Modes and requests

`TILEGEN_L2_DATA_POLICY=sector32|old128`; the new CLI defaults to `sector32`. Direct C++ `DiagnosticOptions` keeps `OLD128` as its default for source compatibility; `Runner` explicitly selects the CLI policy.

- `old128`: the previous 128 B read-fill/store-RFO policy and ordered DRAM records are retained. Additional accounting is passive. The regression compares every original counter and every native DRAM record after 5,000 deterministic mixed accesses against the frozen old core.
- `sector32`: each 128 B tag carries four independent 32-bit known-byte masks and four dirty-byte masks. A store allocates without read, ORs only its actual byte coverage, and marks touched sectors dirty. Touching every sector is not full byte coverage.
- An ordinary read of an incomplete sector fetches exactly 32 B, merges with locally known bytes, and makes that sector readable without expanding the dirty-byte mask. This follows sector readability: even a read targeting four already-known bytes in a partially known sector fetches 32 B.
- An atomic RMW takes one original warp instruction and one forwarded L2 tick per coalesced line, first ensuring each touched sector readable, then marking the actual operand bytes dirty. It cannot become a pure lazy store. A resident fully known sector needs no new DRAM read.
- L1 requested and forwarded masks remain distinct. A request with one L1 sector hit and one miss forwards only the missing sector. The 128 B tag is not treated as four ready L1 sectors.
- Dirty eviction, age cleanup, and explicit drain issue a **32 B masked write transaction**, with the accumulated actual dirty-byte mask. They do not issue an invented pre-write merge read. Read merge does not expand that mask. This corrects the initial task shorthand that partial writeback necessarily needs an RFO, in accordance with the pinned Accel-Sim contract.
- Age/drain preserve known-byte masks and clean tag/LRU position. Thus a partially known, now-clean sector remains unreadable and a subsequent read still fetches 32 B. Capacity eviction drops the tag and all knowledge. Re-dirtying a clean sector begins a new owner lifetime.

The native traffic record format still describes address/transaction byte count/cause and has no byte-mask field. The functional core keeps the mask and passes it to `WritebackObserver`, exports enabled-byte/partial-request totals, and includes every mask in `writeback_byte_mask_fnv1a64`. The old-format postcache hash alone is therefore not a complete mask-identity proof for the new mode. No memory value simulator or standalone value-replay trace is claimed.

With an identical source stream and no drain intervention, this isolation keeps tag allocation, replacement touches, L1 filtering, line-access age ticks, and dirty-sector creation/expiry rules unchanged. Read transaction size/reason and masked-write semantics change; a write-traffic improvement is not assumed. The owner/drain diagnostics and any future separately qualified replacement arm address write gaps without silently changing this arm.

## Accounting

`DRAM_read_bytes` and `DRAM_read_requests` are exact sums of these non-overlapping reason counters, each with `_bytes` / `_requests` suffix:

- `DRAM_load_fill`: ordinary read of previously unknown sector (old128 ordinary line read in the old mode).
- `DRAM_read_merge`: ordinary read of partially known sector, including clean partial knowledge after masked writeback.
- `DRAM_atomic_read`: sector32 atomic old-value read.
- `DRAM_old_store_RFO` and `DRAM_old_atomic_RFO`: old128 only; old public aliases `DRAM_store_RFO` / `DRAM_atomic_RFO` remain.
- `DRAM_capacity_merge`, `DRAM_age_merge`, `DRAM_drain_merge`: explicit reserved reasons, zero under this pinned masked-writeback policy. Their presence does not claim such reads occurred.

`L2_hits` retains tag-hit semantics, including unreadable-sector misses. `L2_sector_read_hits` / `L2_sector_read_misses` count only read/atomic sectors forwarded past L1. None is advertised as an NCU hit-rate metric.

`DRAM_write_bytes = capacity_eviction_writeback_bytes + age_writeback_bytes + drain_writeback_bytes`.

`32 * dirty_sector_creations = DRAM_write_bytes + 32 * resident_dirty_sectors`.

`writeback_enabled_byte_coverage` is the sum of enabled bytes over writeback transactions, not unique addresses, not source requested-byte multiplicity, and not per-byte semantic ownership. `masked_writeback_requests` counts masks that are not full32. Old128's writeback has a full32 enabled mask.

## Phase ownership and explicit drain

Each resident dirty sector stores first/last writer operation ordinal (plus the existing compact role IDs). Runner keeps a separate ordinal-to-phase/module table. It aggregates writebacks by first/last writer phase/module, trigger phase/module, and `capacity|age|drain` reason. Ownership means **first and last touch in one dirty-sector lifetime**, not ownership of every byte in a mixed sector. Phase snapshots expose cumulative writeback flows and resident carry rows; first/last writer can precede the trigger phase.

The JSON field `dirty_ownership` appears in phase snapshots, run_end, and summary:

```
{
  "scope": "first/last touch of dirty sector lifetime; not per-byte ownership",
  "operation_ordinal_phase_table_entries": 2,
  "writebacks_cumulative": [{
    "first_writer_phase": "Warmup/Prefill",
    "last_writer_phase": "Measured/Prefill",
    "trigger_phase": "Diagnostic/run-end",
    "reason": "drain",
    "first_writer_semantic": "...",
    "last_writer_semantic": "...",
    "trigger_semantic": "explicit_drain",
    "write_bytes": 32,
    "enabled_write_byte_coverage": 2,
    "masked_writeback_requests": 1,
    "writeback_merge_read_bytes": 0
  }],
  "resident_dirty_carry": [],
  "resident_dirty_bytes": 0
}
```

No kernel/API/phase/end boundary drains dirty state automatically. An explicit command is accepted only outside an active operation:

```
{"type":"drain","label":"diagnostic-drain","phase":"Diagnostic/run-end"}
```

It emits a separate `type=drain` record with before/after/delta and ownership before/after. It marks `diagnostic_not_natural_ROI=true`, `explicit_dirty_drain=true`, and `clean_tags_and_LRU_preserved=true`. It does not advance the age clock, modify source memory effects, invalidate tags, or touch LRU. Repeated empty drains add no traffic. `end_flush=false` / `no_end_flush=true` retain their previous **no implicit end flush** meaning; `no_implicit_end_flush` and `explicit_drain_performed` remove ambiguity. A measured-phase-end drain changes later cache history and is a separately labeled diagnostic experiment.

## Tests

- `sector32_cache_test.cpp`: hand-computable partial/full/cross-line writes; four partial writes completing one sector; read merge preserving dirty mask; partial capacity/age/drain writeback; same-tick last-store refresh; cold/resident/partially known atomic; L1 partial-hit forwarding; dirty conservation; old128 ordered-record and original-counter regression; cross-phase first/last/trigger owner and no automatic flush.
- `sector32_cli_test.py --binary ELF --output FRESH`: the actual main executable in sector32 and old128, unchanged source-effect projection, no-drain continuous mode, separate terminal drain, invalid policy and nested-drain rejection. Synthetic addresses only; not a full native model run.

Build with C++20 and the same four include roots as `llm/CPP-L1-ADAPTER.md`. Component/actual-main sanitizer receipts live in `build/sector32-components-r1` and the final tracked `sector32-results.json`.
