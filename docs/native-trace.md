# Native/direct binary request trace, version 1

`source/native_trace.h` writes a bounded, ordered **post-cache request** stream.
It does not record each GPU load/store or the device's individual DRAM commands.
The two modes share an encoding; they do not claim identical cache order or timing.

* `NATIVE_COSIM_ADMITTED_REQUESTS`: a record is appended at
  `MemoryPathBackend::bind`, exactly once after the inner backend accepts it.
  Rejected attempts emit nothing. Original issue time and actual physical-service
  admission time are distinct. These are simulated times, not host elapsed time.
* `FUNCTIONAL_DIRECT_DETERMINISTIC_ORDER`: the selected native memory program is
  consumed in a declared deterministic order with functional cache service. All
  four timing fields are `UINT64_MAX`; this mode has no GPU execution timing.

The read cause combines load fill and store RFO: the native request type cannot
distinguish them. Reads are aligned 128-byte requests; each dirty write is an
aligned 32-byte request. Multiple dirty sectors produce separate records.
There is no invented end-of-run dirty-cache flush. A closed trace can therefore
coexist with resident dirty sectors, which are reported by the cache separately.

`source_matrix_id` and `source_line_address` preserve the request's cache key
(possibly already canonicalized). `service_address` is the mapped address sent
to the native memory service or the explicitly declared direct namespace.
Neither is automatically an observed GPU physical address. For writebacks,
node/CTA/warp/PC metadata describes the eviction trigger where available, not
the last store that produced the victim's bytes. Unknown metadata is not inferred.
Native records currently leave CTA/warp/PC unknown; functional direct leaves the
L2 subpartition unknown because it does not reproduce GPU scheduling.

## Encoding

Every integer is an explicit **unsigned 64-bit little-endian** word. There is no
serialized C++ padding, platform-native endianness, or implicit variable width.
Signed source IDs are preserved by conversion modulo 2^64; `-1` is the unknown
sentinel `UINT64_MAX`. Call indices and request IDs must be known.

The 96-byte header is:

| Offset | Value |
| --- | --- |
| 0 | Eight ASCII bytes `TGCSIM01` |
| 8 | Record width, 144 |
| 16 | Mode: 0 native cosim, 1 functional direct |
| 24 | Reserved zero |
| 32 | 64 lowercase ASCII hex characters: caller-supplied context SHA-256 |

Each 144-byte record consists of these 18 words, in order:

1. `request_id`
2. `source_sequence`
3. `issue_cycle`
4. `issue_ps`
5. `admission_cycle`
6. `admission_ps`
7. `source_matrix_id`
8. `source_line_address`
9. `service_address`
10. `bytes`
11. `node_id`
12. `sm_id`
13. `l2_subpartition_id`
14. `cause` (0 read fill/RFO, 1 dirty writeback)
15. `call_index`
16. `cta`
17. `warp`
18. `pc`

The 128-byte footer begins with `UINT64_MAX` (an illegal next request ID), then
eight ASCII bytes `TGCSEND1`. At byte 16 are six words: record count, read count,
write count, read bytes, write bytes, request-payload FNV-1a-64. At byte 64 is a
64-character lowercase SHA-256 over record bytes only. FNV processes each
request's `(request_id, service_address, bytes, is_write)` as four little-endian
words, matching the native backend's existing request-shape hash. A separate
whole-file SHA-256 is returned in the JSON receipt.

This is a new format. It does **not** claim binary compatibility with the older
`M8TRC001` or `TGSF0001` streams.

In this executable, `context_sha256` hashes the decoded transport control and
frame declarations. It does not hash command-line scheduling flags or the
binary. Keep `result.json` and `run-receipt.json` with the trace: they identify
`cosim` versus approximate `cosim-fast`, the timing profile and binary SHA.
Both cosimulation profiles use header mode 0; that value alone does not certify
all-fine scheduling. An independent consumer must check the associated metadata.

## API and completion

```cpp
native_trace::Writer writer(path, native_trace::Mode::NativeCosim,
                            context_sha256, max_bytes);
backend.set_trace_sink(&writer);       // before the first accepted request
backend.set_trace_context(call_index); // at each drained kernel boundary
// Run and finalize the native simulator/backend.
const auto receipt = writer.finish();
const auto json = receipt.to_json();
```

The constructor creates only `path + ".partial"`, refusing existing names.
`finish()` writes the footer, flushes and fsyncs, then independently reads all
serialized records back through the validator. Only after successful validation
does an atomic same-directory hard link publish `path` without replacing an
existing file; the partial name is then removed. An interrupted/failed writer
does not publish a final name or return a successful receipt. Its partial file
may contain only bytes already flushed; it is not qualified input.

Both reader and writer use a fixed 64 KiB buffer and constant-size counters.
The API default is 16 GiB and the permitted cap is 224 bytes through 64 GiB.
The cap includes header, records and footer; reaching it is a hard failure.
It is not evidence that a complete model workload fits the configured quota.
The uncompressed encoding uses 144 bytes per request plus 224 bytes per file.

```cpp
auto checked = native_trace::validate(path, trusted_file_sha256, max_bytes,
    [&](const native_trace::Record& record) { /* inspect provisional record */ });
```

The reader requires an externally trusted whole-file SHA. It validates the
header, mode, shapes, contiguous request IDs, nondecreasing call indices, native
time ordering or direct unknown times, census, record SHA, exact EOF, file size,
and whole-file SHA. Visitor callbacks occur before final validation; their data
must remain provisional until `validate()` returns successfully. The caller must
also match `context_sha256` to its intended input/configuration. A successful
readback establishes trace serialization and accounting integrity, not the
physical accuracy of the source model or equality between the two modes.

## CPU test scope

`tests/native_trace_test.cpp` checks 5,000-record round trips in each mode,
corruption/truncation/trailing bytes, invalid shapes/identities/context/timing,
file quota and no-overwrite behavior. It also runs the actual native memory
backend with a small physical queue, in immediate and delayed ingress modes,
with trace on and off. It compares complete completion sequences, simulated
cycles, admitted payload hashes, path accounting, and physical service counts
and timing. These bounded CPU fixtures do not qualify a full LLM workload.
