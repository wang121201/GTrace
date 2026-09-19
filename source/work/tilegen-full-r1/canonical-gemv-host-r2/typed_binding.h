#pragma once
#include "../canonical-gemv-driver-r2/model_plan.h"
#include "../../../native_typed_identity.h"

namespace canonical_gemv::typed {

// This identity describes the host capture, independently of the old program
// template. The caller must verify its receipt/journals before constructing it.
// Recording these hashes does not authenticate a capture or qualify transfer.
using native_typed::CaptureIdentity;
using native_typed::hash_text;
struct Parameters {
    CaptureIdentity capture;
    std::array<std::uint32_t,38> words{};
    std::string code_sha256,parameter_layout_sha256;
    std::array<U,3> grid{},block{};
    U static_shared_bytes=0,dynamic_shared_bytes=0,registers=0,local_bytes_per_thread=0;
};
struct ObjectRebase { U source_base,target_base,target_bytes; };

// A lease on the existing sealed source, not a lease on a new target's dynamic
// behavior. SourceCatalog and its immutable SourceBundle outlive all borrowers.
class TemplateLease {
    const SourceBundle& source_;
    const std::string id_;
    const J pin_,template_;
    static J pin_for(const std::string& id) {
        const auto fixed=seals();
        p::need(fixed.at("template_files").contains(id),"typed GEMV unknown sealed template");
        return fixed.at("template_files").at(id);
    }
public:
    TemplateLease(const SourceCatalog& catalog,const std::string& id)
        :source_(catalog.get(id)),id_(id),pin_(pin_for(id)),template_(strict_parse(check_pin(pin_))) {
        const auto& t=template_;
        p::need(t.at("memory_program")==source_.input.at("program_file")&&
                t.at("register_program")==source_.input.at("register_file"),
                "typed GEMV sealed program/register source identity");
        // Do not trust a caller-supplied SourceBundle carrying only pin labels.
        p::need(source_.input.at("program")==strict_parse(check_pin(t.at("memory_program")))&&
                source_.input.at("register_program")==strict_parse(check_pin(t.at("register_program"))),
                "typed GEMV immutable source content differs from sealed files");
        p::need(t.at("grid")==source_.input.at("register_program").at("grid")&&
                t.at("counts").at("nodes_per_CTA")==source_.registers.nodes.size()&&
                t.at("object_roles").size()==source_.program.objects.size(),
                "typed GEMV original source geometry/census");
        p::need(t.at("explicit_dataflow").at("tensor_controls_observed_addresses_guards_PC")==false&&
                t.at("explicit_dataflow").at("relocated_pointer_controls_observed_guards_PC")==false,
                "typed GEMV preserves unknown implicit/data-dependent transfer");
        p::need(source_.resident==int(p::natural(t.at("native_resources").at("max_active_blocks_per_sm")))&&
                source_.resident>0,"typed GEMV original native resource limit");
    }
    const SourceBundle& source()const{return source_;}
    const J& value()const{return template_;}
    J identity()const{return {{"template_id",id_},{"template_pin",pin_},
        {"memory_program",source_.input.at("program_file")},
        {"register_program",source_.input.at("register_file")},
        {"source_process",source_.identity}};}
};

// Validates only the closed GEMV address domain. This is deliberately separate
// from Model's sealed old process/phase/call admission; no new call enters it.
class Binding {
    const SourceBundle& source_;
    const Parameters parameters_;
    std::vector<ObjectRebase> objects_;
    J source_identity_;
    U K_=0,N_=0,ctas_=0;
    static U pointer(const Parameters& v,std::size_t word) {
        return U(v.words.at(word))|(U(v.words.at(word+1))<<32);
    }
public:
    Binding(const TemplateLease& lease,const Parameters& value)
        :source_(lease.source()),parameters_(value),source_identity_(lease.identity()) {
        const auto& t=lease.value();const auto& c=value.capture;
        p::need(c.pid>0&&c.start_ticks>0&&c.native_launch_id>0&&
            !c.source_launch_key.empty()&&c.source_launch_key.size()<=128&&
            !c.phase.empty()&&c.phase.size()<=128&&hash_text(c.argument_payload_sha256)&&
            hash_text(c.capture_receipt_sha256),"typed GEMV explicit host-capture provenance");
        p::need(value.code_sha256==t.at("code_sha256").get<std::string>()&&
                value.parameter_layout_sha256==t.at("parameter_layout_sha256").get<std::string>(),
                "typed GEMV exact source code/152B ABI");
        for(U i=0;i<3;++i)p::need(value.grid[i]==p::natural(t.at("grid").at(i))&&
                value.block[i]==p::natural(t.at("block").at(i)),"typed GEMV exact source geometry");
        const auto& resources=t.at("native_resources");
        p::need(value.static_shared_bytes==p::natural(resources.at("static_shared_bytes"))&&
                value.dynamic_shared_bytes==p::natural(resources.at("dynamic_shared_bytes"))&&
                value.registers==p::natural(resources.at("registers"))&&
                value.local_bytes_per_thread==p::natural(resources.at("local_bytes_per_thread")),
                "typed GEMV exact measured resources");
        for(U i=0;i<38;++i)if(i!=0&&i!=1&&i!=4&&i!=5&&i!=8&&i!=9&&i!=12&&i!=13)
            p::need(U(value.words[i])==p::natural(t.at("argument_words").at(i),UINT32_MAX),
                    "typed GEMV all120 nonpointer bytes exact");
        p::need(pointer(value,8)==pointer(value,12),"typed GEMV output/epilogue alias");
        K_=p::natural(t.at("K"));N_=p::natural(t.at("N"));ctas_=value.grid[0];
        p::need(K_>0&&N_>0&&ctas_>0&&value.grid[1]==1&&value.grid[2]==1&&
                N_==p::multiply(ctas_,value.block[1])&&value.words[35]>0&&
                K_%p::multiply(value.block[0],U(value.words[35]))==0,
                "typed GEMV complete no-tail domain");
        p::need(ctas_==source_.program.ctas,"typed GEMV exact original full CTA domain");
        std::map<std::string,std::pair<U,U>> roles;
        for(auto [role,index]:std::array<std::pair<const char*,U>,3>{{{"weight",0},{"input",4},{"output",8}}}) {
            const std::string name=role;const U base=pointer(value,index);
            const U extent=name=="weight"?p::multiply(2,p::multiply(K_,N_)):
                p::multiply(2,name=="input"?K_:N_);
            p::need(base>0&&base%128==p::natural(t.at("pointer_alignment_mod128").at(name)),
                    "typed GEMV pointer alignment");
            (void)p::add(base,extent); // Allocation/root coverage remains unproven.
            roles.emplace(name,std::make_pair(base,extent));
        }
        const auto& role_map=t.at("object_roles");objects_.reserve(role_map.size());
        for(U i=0;i<role_map.size();++i) {
            const auto role=role_map.at(i).get<std::string>();
            p::need(roles.count(role)==1,"typed GEMV sealed source object role");
            const U origin=t.contains("object_argument_bases")?
                p::natural(t.at("object_argument_bases").at(i)):source_.program.objects.at(i).base;
            const auto target=roles.at(role);objects_.push_back({origin,target.first,target.second});
        }
        p::need(objects_.size()==source_.program.objects.size(),"typed GEMV object domain closure");
    }
    const SourceBundle& source()const{return source_;}
    const std::vector<ObjectRebase>& objects()const{return objects_;}
    U ctas()const{return ctas_;}
    J evidence()const {
        const auto& c=parameters_.capture;
        return {{"schema","GEMV_TYPED_ADDRESS_DOMAIN_V1"},{"status","VALIDATED_PARAMETER_DOMAIN_ONLY"},
            {"source_template",source_identity_},{"host_capture",{{"pid",c.pid},{"start_ticks",c.start_ticks},
                {"native_launch_id",c.native_launch_id},{"source_launch_key",c.source_launch_key},
                {"phase",c.phase},{"argument_payload_sha256",c.argument_payload_sha256},
                {"capture_receipt_sha256",c.capture_receipt_sha256}}},
            {"K",K_},{"N",N_},{"CTAs",ctas_},{"nonpointer_bytes_checked",120},
            {"host_capture_files_verified_by_this_constructor",false},
            {"expected_extents_are_observed_allocations",false},{"object_root_binding_validated",false},
            {"dynamic_memory_validated",false},{"register_control_transfer_validated",false},
            {"native_model_admitted",false},{"implicit_dependencies_complete",false}};
    }
};
} // namespace canonical_gemv::typed
