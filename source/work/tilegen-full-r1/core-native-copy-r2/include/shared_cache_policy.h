#pragma once
#include "source_semantics.h"
#include "ef_hit_throttle.h"
#include <limits>
#include <stdexcept>

namespace GTSim::shared_cache_policy {
struct State {
    direct_native::EfHitThrottle selector;
    std::uint64_t processed_hits=0, known_non_EF_read_hits=0, unknown_read_hits=0, store_hits=0;
    std::uint64_t completed_fill_candidates=0, inspected_waiters=0, pure_EF_fills=0, fallback_fills=0;
    std::uint64_t new_EF_lines_inserted_lru=0, existing_line_fills=0;
    explicit State(unsigned numerator):selector(numerator) {
        if(numerator!=0&&numerator!=288&&numerator!=65536)
            throw std::invalid_argument("fixed shared cache profile must use h0/h288/h65536");
    }
    static void add(std::uint64_t& n,std::uint64_t amount=1) {
        if(amount>std::numeric_limits<std::uint64_t>::max()-n)
            throw std::overflow_error("shared policy count overflow");
        n+=amount;
    }
    template<class Tx> static source_memory::View view(const Tx& tx) {
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
        return tx.source_evidence;
#else
        (void)tx;return {};
#endif
    }
    static bool known_EF_read(source_memory::View v,bool write) {
        return !write&&v.selection==source_memory::Selection::Known&&v.record&&
            v.record->operation=='R'&&v.record->observed_sass==source_memory::ObservedSass::ExactEF;
    }
    // Returns true for an actual move to the LRU end. Never called for misses,
    // pending merges, L1-filtered requests or a completion's synthetic context.
    template<class Tx> bool hit_to_lru(const Tx& tx) {
        add(processed_hits);const auto v=view(tx);
        if(tx.is_write){add(store_hits);return false;}
        if(known_EF_read(v,false)) {
            if(selector.eligible()==std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("shared selector population overflow");
            return !selector.choose();
        }
        if(v.selection==source_memory::Selection::Known&&v.record)add(known_non_EF_read_hits);
        else add(unknown_read_hits);
        return false;
    }
    template<class Waiting> bool fill_at_lru(const Waiting& waiting) {
        add(completed_fill_candidates);add(inspected_waiters,waiting.size());bool pure=!waiting.empty();
        for(const auto& tx:waiting)pure=pure&&known_EF_read(view(tx),tx.is_write);
        add(pure?pure_EF_fills:fallback_fills);return pure;
    }
    template<class J> J report() const {
        return {{"processed_hits",processed_hits},{"eligible_EF_read_hits",selector.eligible()},
            {"promoted_to_MRU",selector.promoted()},{"moved_to_LRU",selector.eligible()-selector.promoted()},
            {"known_non_EF_read_hits",known_non_EF_read_hits},{"unknown_read_hits",unknown_read_hits},{"store_hits",store_hits},
            {"completed_fill_candidates",completed_fill_candidates},{"inspected_waiters",inspected_waiters},
            {"pure_EF_fills",pure_EF_fills},{"fallback_fills",fallback_fills},
            {"new_EF_lines_inserted_lru",new_EF_lines_inserted_lru},{"existing_line_fills",existing_line_fills}};
    }
};
} // namespace GTSim::shared_cache_policy
