#pragma once
#include <span>
#include "source_semantics.h"
namespace GTSim::source_memory {
// A view is valid only while its immutable SourceBundle remains alive. This
// accessor neither copies a string nor retains request/node/span references.
inline View select(std::span<const ExplicitMemorySubop> subops,int index) noexcept {
#if TILEGEN_SOURCE_MEMORY_SEMANTICS
    if(index == -1) {
        if(subops.empty()) return {nullptr,Selection::Missing};
        if(subops.size()!=1) return {nullptr,Selection::Ambiguous};
        index=0;
    }
    if(index<0||std::size_t(index)>=subops.size())return {nullptr,Selection::InvalidIndex};
    const auto* record=subops[std::size_t(index)].source_semantics;
    return {record,record?Selection::Known:Selection::Missing};
#else
    (void)subops;(void)index;return {nullptr,Selection::Disabled};
#endif
}
}
