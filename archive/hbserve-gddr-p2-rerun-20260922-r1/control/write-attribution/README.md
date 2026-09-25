# Current-history dirty writer attribution

SOURCE READY only. No cache simulation or large build was run. The change is one constructor line in an isolated copy of the frozen `writer_observer.h`: a fifth `max_calls=1138` argument replaces the literal range limit. Default four-argument callers keep their original accepted domain. All fields and all other methods remain byte exact. No macro is necessary.

## Integration

Compile the driver and every TU that includes the writer definition with the same `-ivfsoverlay <this-directory>/overlay.json`. The overlay redirects exactly the frozen shared-h288 snapshot's absolute writer header to the isolated header. `use-external-names=false` preserves the virtual include path and sibling includes. `history_attribution.h` includes that same virtual absolute path; do not separately include the physical replacement by another path. Frozen files are not modified.

```cpp
#include "<this-directory>/history_attribution.h"
// Device and its L2 have already been constructed. No modeled work yet.
current_history_write::Session<g::L2Cache> writer(d.cache, plan.at("timeline").size());
for (const auto& e : plan.at("timeline")) {
    writer.set_operation(rows.size());
    // Existing dispatch, including all metadata nodes; retain label through drain.
    // Existing row append remains in its original position.
}
// Existing driver must have drained L2 and the physical backend.
writer.finish();
result["writer_attribution"] = writer.report<J>();
```

The wrapper permits 1..3020 selected nodes; the frozen complete timeline has 3018. `call_index` is the dense selected timeline ordinal, **not** the native launch ID or source submission event. Join it to the existing row/timeline identity. Empty metadata nodes have zero mutation counts. Use the actual selected prefix length: this is one continuous observer for its entire selected history, with no subdivision or reseed. Construct the device first and keep it alive longer than the session. A failed run abandons attribution; it cannot emit a closed report. There is no end flush.

The wrapper uses `Observation(0, selected_nodes-1, false, nullptr, 3020)`. The optional all-history unique-address sets are deliberately disabled: the original 2,000,000-sector history cap is inappropriate for the approximately 16 GB initialization API writes. The original live-line bound, supplied by L2 as cache lines + backend admission capacity + pending DRAM capacity, remains unchanged (and must be <=400,000). There is no per-operation cache scan: entry and exit enumerate dirty state; live mutation hooks update the bounded map. The 3018 count rows contain nine uint64 counters each (217,296 bytes excluding vector bookkeeping); map allocation/host overhead remains to be measured.

## Attribution that is actually available

The original hook maintains each dirty sector's first 0-to-1 writer, most recent writer, and whether multiple observed operations wrote during that dirty lifetime. It counts dirty eviction by the operation active when the eviction occurs. This is an **eviction triggering operation**, which can be a kernel or API, not necessarily a semantic consumer/read. It emits first-writer, last-writer and trigger marginals plus final resident stock. It does not retain every intermediate writer, a creator-by-trigger joint table, tensor byte ownership, or complete trace. Do not cross-join the marginals into an invented causal table. Entry-inherited dirtiness retains its explicitly unknown creator; cold entry should have I=0, but no dirty seed is synthesized.

For the fixed mode2, non-bypass L2, exact store hooks exist on resident store HIT (`memory.h:2349`), fresh write MSHR (`:2382`), and write merge (`:2404`). Actual dirty-victim writeback invokes the eviction hook at `:2587`; each dirty sector is a 32 B backend request. `decode_writer_begin/end` (`:1261/:1272`) join native store/C/E and issued/completed writeback bytes, compare all final dirty masks, and enforce I+C=E+F plus attribution marginal closure. Accepted but not yet processed requests do not update writer attribution.

`FineContext` clears only the separate `L2RuntimeObserver*` at `fine_context.h:150/:156`; its entry/exit do not clear the independent thread-local `decode_writer::Scope`. The direct writer scope therefore survives Fine, Tiny and API dispatch. `ApiPort` uses actual L2LineRequest writes with exact range masks and bypasses L1; these enter the same writer hooks. Existing API/kernel quiescence before advancing the operation label is essential: asynchronous cross-operation dispatch would make this label semantics insufficient and is not admitted here. DMA cache coherence and operation quiescence remain existing model assumptions, not new hardware claims.

## Build boundary and regression

Frozen `.d` files show consumers 00..06 include the writer header: old driver 00, `gpu.cpp`, `scheduler.cpp`, `simulator.cpp`, `simulator_session.cpp`, `sm.cpp`, and `subpartition.cpp`. Replace old driver 00 with the new current-history driver and rebuild core 01..06 under the identical overlay. Reusing old 01..06 would mix different inline class definitions despite unchanged layout (ODR violation). HBFSIM objects 07..21 have no dependency on the writer header and may be reused only while their full dependency/configuration/compiler pins remain valid. The final driver `.d` must confirm all writer includes resolve to the one expected virtual header; all compile steps pin both overlay and physical replacement because a dependency file may retain only the virtual name.

Historical shared-h288 compilation times are recorded in `source-proof.json` (01..06 totaled about 12.22 s; old driver 00 about 17.34 s, with concurrent jobs). These are prior observations, not a prediction for the new driver or total build. A new build and link are still required. No observer-enabled model regression has yet been performed here.

Before a full observed history, root should compare the same bounded source/input/config/binary settings with observer disabled/enabled, excluding only attribution/host-cost fields. All modeled cycles, work, traffic, callback/queue closure, dirty state and policy counters must remain exact. Report observer host cost separately. The static checks delivered here prove the limited source change and dependency classification, not runtime passivity or hardware accuracy.
