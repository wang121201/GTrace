#pragma once
// Passive source evidence. No cache/scheduler/backend consumes these values.
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <cstddef>

#ifndef TILEGEN_SOURCE_MEMORY_SEMANTICS
#define TILEGEN_SOURCE_MEMORY_SEMANTICS 0
#endif
#if TILEGEN_SOURCE_MEMORY_SEMANTICS != 0 && TILEGEN_SOURCE_MEMORY_SEMANTICS != 1
#error TILEGEN_SOURCE_MEMORY_SEMANTICS must be 0 or 1
#endif

namespace GTSim::source_memory {
enum class ObservedSass : std::uint8_t { Uninterpreted=0, ExactEF=1 };
enum class ResolvedL1Route : std::uint8_t { Unknown=0, Bypass=1, Eligible=2 };
enum class ExplicitL2Policy : std::uint8_t { Unknown=0, Normal=1, EvictFirst=2, EvictLast=3 };
enum class Selection : std::uint8_t { Known=0, Missing=1, Ambiguous=2, InvalidIndex=3, Disabled=4 };
struct Origin {
    std::string template_source_key, code_sha256;
    std::uint64_t source_cta=0;
    std::uint64_t source_process_id=0, source_start_ticks=0;
};
struct Record {
    const Origin* origin=nullptr;
    const std::string* raw_opcode=nullptr;
    std::uint32_t body_record_index=0, warp_memory_ordinal=0, warp=0;
    std::uint64_t pc=0, function=0;
    std::uint32_t effective_mask=0, width_bytes=0, lane_count=0;
    char operation=0;
    ObservedSass observed_sass=ObservedSass::Uninterpreted;
    ResolvedL1Route resolved_l1=ResolvedL1Route::Unknown;
    ExplicitL2Policy explicit_l2=ExplicitL2Policy::Unknown;
};
struct View {
    const Record* record=nullptr;
    Selection selection=Selection::Missing;
};

// Owned once by SourceBundle. SourceCatalog holds unique_ptr<const SourceBundle>
// through all call providers, DAG nodes, admission and drain. Never copy/move:
// published Record*, Origin* and opcode pointers have that exact owner lifetime.
class Table final {
    Origin origin_;
    std::vector<std::string> opcodes_;
    std::vector<Record> records_;
    static void need(bool b,const char* why) { if(!b)throw std::invalid_argument(why); }
    template<class J> static std::uint64_t natural(const J& j,std::uint64_t cap) {
        need(j.is_number_integer()&&!j.is_boolean(),"source semantics integer");
        if(!j.is_number_unsigned())need(j.template get<std::int64_t>()>=0,"source semantics negative");
        const auto v=j.template get<std::uint64_t>();need(v<=cap,"source semantics bound");return v;
    }
public:
    Table(const Table&)=delete;Table& operator=(const Table&)=delete;
    Table(Table&&)=delete;Table& operator=(Table&&)=delete;
    template<class J> explicit Table(const J& program) {
        need(program.at("schema")=="TILEGEN_NATIVE_CTA_MEMORY_PROGRAM_V1","source semantics schema");
        const auto& begin=program.at("source").at("begin");
        origin_.template_source_key=begin.at("source_launch_key").template get<std::string>();
        origin_.code_sha256=begin.at("code_sha256").template get<std::string>();
        need(!origin_.template_source_key.empty()&&origin_.template_source_key.size()<=128&&
             origin_.code_sha256.size()==64,"source semantics source/code identity");
        const auto& binding=program.at("source").at("native_binding");
        origin_.source_process_id=natural(binding.at("source_process_id"),UINT64_MAX);
        origin_.source_start_ticks=natural(binding.at("source_start_ticks"),UINT64_MAX);
        need(origin_.source_process_id&&origin_.source_start_ticks&&
             natural(binding.at("source_launch_key").at("pid"),UINT64_MAX)==origin_.source_process_id&&
             natural(binding.at("source_launch_key").at("start_ticks"),UINT64_MAX)==origin_.source_start_ticks,
             "source semantics process epoch binding");
        const auto& bodies=program.at("programs");
        need(bodies.is_array()&&bodies.size()==1,"source semantics single qualified body");
        origin_.source_cta=natural(bodies.at(0).at("source_cta"),UINT32_MAX);
        const auto& ops=program.at("opcodes");
        need(ops.is_array()&&!ops.empty()&&ops.size()<=4096,"source semantics opcode bound");
        opcodes_.reserve(ops.size());
        for(const auto& op:ops) {
            auto s=op.template get<std::string>();
            need(!s.empty()&&s.size()<=256,"source semantics opcode text");opcodes_.push_back(std::move(s));
        }
        const auto& rows=bodies.at(0).at("records");
        need(rows.is_array()&&!rows.empty()&&rows.size()<=32768,"source semantics record bound");
        records_.reserve(rows.size());std::map<std::uint32_t,std::uint32_t> ordinals;
        for(const auto& row:rows) {
            Record r;r.origin=&origin_;
            r.body_record_index=static_cast<std::uint32_t>(records_.size());
            r.warp=static_cast<std::uint32_t>(natural(row.at("warp"),31));
            r.warp_memory_ordinal=ordinals[r.warp]++;
            r.pc=natural(row.at("pc"),UINT64_MAX);r.function=natural(row.at("function"),UINT64_MAX);
            r.raw_opcode=&opcodes_.at(natural(row.at("opcode"),opcodes_.size()-1));
            r.effective_mask=static_cast<std::uint32_t>(natural(row.at("effective_mask"),UINT32_MAX));
            r.width_bytes=static_cast<std::uint32_t>(natural(row.at("width"),16));
            need(r.width_bytes==1||r.width_bytes==2||r.width_bytes==4||r.width_bytes==8||r.width_bytes==16,"source semantics width");
            auto op=row.at("op").template get<std::string>();need(op=="R"||op=="W","source semantics operation");r.operation=op[0];
            need(row.at("lanes").is_array()&&row.at("lanes").size()<=32,"source semantics lane bound");
            r.lane_count=static_cast<std::uint32_t>(row.at("lanes").size());
            r.observed_sass=*r.raw_opcode=="LDG.E.EF.U16"?ObservedSass::ExactEF:ObservedSass::Uninterpreted;
            // Deliberately no inference from EF, STRONG.SM, direction, role or
            // simulator bypass_l1. These captures do not resolve either field.
            records_.push_back(r);
        }
    }
    const Record* at(std::size_t i) const { return &records_.at(i); }
    std::size_t size() const noexcept { return records_.size(); }
    const Origin& origin() const noexcept { return origin_; }
};
} // namespace GTSim::source_memory
