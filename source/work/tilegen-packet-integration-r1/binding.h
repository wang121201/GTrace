#pragma once
#include "source_support.h"
#include "../tilegen-full-r1/canonical-gemv-host-r2/prepared_address.h"
#include <unordered_set>

namespace packet_binding {
using namespace native_sequence;

// A bound memory instruction, built on demand for one active source member.
// The immutable program stays shared. No DAGNode or complete CTA is allocated.
struct MemoryInstruction {
    bool global = false, write = false, zero_lane_compute = false;
    U logical_bytes = 0;
    std::vector<g::ExplicitMemorySubop> subops;
    std::vector<g::CacheLineKey> lines;
    g::ExplicitSRAMService shared_service{};
};

struct CallBinding {
    const canonical_gemv::Model& model;
    const J& call;
    const SourceBundle& source;
    canonical_gemv::PreparedAddress addresses;
    const coupling::ServiceMapper& mapper;
    U ctas;

    CallBinding(const canonical_gemv::Model& m,const J& c,
                const coupling::ServiceMapper& map)
        : model(m),call(c),source(m.source(c)),addresses(m,c),mapper(map),
          ctas(m.prefix ? m.prefix : p::natural(c.at("grid")[0])) {}

    const native_register::Node& node(U member) const {
        p::need(member<source.registers.order.size(),"packet member out of source");
        const auto& n=source.registers.nodes.at(source.registers.order.at(member));
        p::need(U(n.output)==member,"packet dense source binding");
        return n;
    }

    MemoryInstruction memory(U cta,U member) const {
        p::need(cta<ctas,"packet CTA out of selected grid");
        const auto& n=node(member);
        p::need(n.kind=="global"||n.kind=="shared","packet member is not memory");
        MemoryInstruction out;out.global=n.kind=="global";out.write=n.op=='W';
        g::ExplicitMemorySubop sub;
        if (out.global) {
            const auto& r=source.program.body(0).records.at(n.memory);
            p::need(r.op==n.op,"packet memory direction");
            out.logical_bytes=p::multiply(r.lanes.size(),U(r.width));
            if (r.lanes.empty()) { out.zero_lane_compute=true;return out; }
            sub.requested_bytes=out.logical_bytes;
            for (const auto& lane:r.lanes) sub.source_member_ordinals.push_back(lane.lane);
            const auto& plan=source.range_plan.records.at(n.memory);
            std::unordered_set<U> seen;
            for (const auto& group:plan) {
                const auto& lane=r.lanes.at(group.first);
                const U va=addresses.address(lane,group.bytes,cta);
                sub.ranges.push_back({group.count==1?lane.lane:-1,va,group.bytes});
                p::need(group.bytes>0&&va<=UINT64_MAX-(group.bytes-1),"packet source range overflow");
                const U last=(va+group.bytes-1)/128*128;
                for (U line=va/128*128;;line+=128) {
                    // Same mapping check for every original range intersection.
                    const g::CacheLineKey key{1,line};(void)mapper.map(key);
                    // Original coalescer: first occurrence in range order.
                    if (seen.insert(line).second) out.lines.push_back(key);
                    if (line==last) break;
                }
            }
            p::need(!out.lines.empty(),"nonempty global source has no lines");
        } else {
            sub.requested_bytes=p::multiply(n.lanes.size(),U(n.width));
            out.logical_bytes=sub.requested_bytes;
            for (auto [lane,address]:n.lanes) {
                sub.ranges.push_back({lane,address,U(n.width)});
                sub.source_member_ordinals.push_back(lane);
            }
            out.shared_service=g::describe_explicit_sram_ranges(sub);
        }
        out.subops.push_back(std::move(sub));
        return out;
    }
};

// This object validates existing canonical source inputs separately from the
// full-workflow service map used by the new bounded comparison. Using a cold
// selected subsequence is explicitly not the original full-run cache state.
struct Input {
    J envelope,full_control,summary;
    std::unique_ptr<canonical_gemv::Model> model;
    std::map<std::string,J> plans;
    unsigned q=0;
    explicit Input(const J& in):envelope(in) {
        keys(in,{"schema","canonical_gemv","full_control_pin","packet_summary_pin","q"});
        p::need(in.at("schema")=="TILEGEN_PACKET_WINDOW_INPUT_V1","packet input schema");
        q=unsigned(p::natural(in.at("q"),16));
        p::need(q==1||q==2||q==4||q==8||q==16,"packet supported q");
        full_control=strict_parse(check_pin(in.at("full_control_pin")));
        summary=strict_parse(check_pin(in.at("packet_summary_pin")));
        p::need(summary.at("status")=="PASS_PURE_TEMPLATE_COMPILATION"&&
                summary.at("component_only_not_standalone_simulator")==true,
                "packet static compilation qualification");
        for(const auto& pin:summary.at("input_pins")) (void)check_pin(pin);
        p::need(in.at("canonical_gemv").at("memory_model")==
                full_control.at("decoded_control").at("memory_model"),
                "packet and full32 memory profiles differ");
        model=std::make_unique<canonical_gemv::Model>(in.at("canonical_gemv"));
        std::set<std::string> required;
        for(auto* c:model->calls) required.insert(c->at("template_key").get<std::string>());
        for(const auto& row:summary.at("templates")) {
            const auto key=row.at("template").get<std::string>();
            if(!required.count(key)) continue;
            const auto& pp=row.at("q").at(std::to_string(q)).at("plan_pin");
            J plan=strict_parse(check_pin(pp));
            J shared=strict_parse(check_pin(plan.at("shared_program")));
            const auto& source=model->catalog->get(key);
            p::need(plan.at("schema")=="TILEGEN_SHARED_COMPUTE_PACKET_PLAN_V1"&&
                    plan.at("q_core_cycles")==q&&plan.at("template")==key,
                    "packet plan identity");
            p::need(plan.at("shared_program")==row.at("shared_program")&&
                    shared.at("source_pins").at("register")==source.input.at("register_file")&&
                    shared.at("source_pins").at("memory_program")==source.input.at("program_file"),
                    "packet source program identity");
            p::need(plan.at("statistics").at("logical_nodes")==source.registers.nodes.size()&&
                    shared.at("nodes").size()==source.registers.nodes.size(),
                    "packet source member census");
            for(std::size_t i=0;i<source.registers.order.size();++i) {
                const auto& n=source.registers.nodes.at(source.registers.order[i]);
                const auto& s=shared.at("nodes").at(i);
                p::need(s[0]==i&&s[1]==n.warp&&s[2]==n.local&&s[3]==n.kind&&
                        s[5]==n.elements&&s[6]==n.pc&&s[7]==n.function&&
                        s[8]==n.active&&s[9]==n.mask,"packet member source metadata");
            }
            plan["shared_source_index"]=std::move(shared);
            p::need(plans.emplace(key,std::move(plan)).second,"duplicate packet template");
        }
        p::need(plans.size()==required.size(),"missing selected packet template");
    }
    J memory_envelope() const {
        J result=envelope.at("canonical_gemv");
        result["service_address_map"]=full_control.at("decoded_control").at("service_address_map");
        return result;
    }
    void finish() const {
        model->catalog->seals.finish();
        (void)check_pin(envelope.at("full_control_pin"));
        (void)check_pin(envelope.at("packet_summary_pin"));
    }
};
} // namespace packet_binding
