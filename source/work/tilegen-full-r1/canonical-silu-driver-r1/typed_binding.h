#pragma once
#include "prepared_memory.h"
#include "../../../native_typed_identity.h"

namespace canonical_silu {
// A source lease validates the existing memory/register/template artifacts. It
// grants no transfer qualification to a new target process or launch. Its
// SourceBundle/catalog must outlive the lease; completed bindings own ranges.
class TypedSourceLease {
    const SourceBundle& source_;
    std::vector<Formula> formulas_;
    J pins_;
    friend class TypedBinding;
public:
    TypedSourceLease(const SourceBundle& source,const J& template_pin):source_(source) {
        const J fixed=seals();
        p::need(source.input.at("program_file")==fixed.at("program_file")&&
                source.input.at("register_file")==fixed.at("register_file")&&
                template_pin==fixed.at("template_file"),"SiLU typed explicit fixed source/template lease");
        pins_={{"program",source.input.at("program_file")},{"registers",source.input.at("register_file")},{"template",template_pin}};
        (void)check_pin(pins_.at("program"));(void)check_pin(pins_.at("registers"));
        const J templ=strict_parse(check_pin(template_pin));
        p::need(templ.at("register_program")==pins_.at("registers")&&
            templ.at("schema")=="SILU_ADDRESS_NEUTRAL_CTA_TEMPLATE_V1"&&
            source.program.warps==32&&source.resident==1&&source.registers.nodes.size()==11800&&
            source.registers.global_nodes==744&&source.registers.shared_nodes==0&&source.registers.barrier_nodes==0,
            "SiLU typed closed source census");
        std::map<std::pair<U,U>,Formula> by_key;
        for(const auto& r:templ.at("memory_records")) {
            const auto op=r.at("op").get<std::string>();p::need(op=="R"||op=="W","SiLU typed source direction");
            Formula f{p::natural(r.at("warp"),31),p::natural(r.at("ordinal"),31),p::natural(r.at("pc")),
                p::natural(r.at("width"),16),p::natural(r.at("occurrence"),5),p::natural(r.at("part_bytes")),
                op[0],r.at("role").get<std::string>(),r.at("opcode").get<std::string>()};
            p::need(by_key.emplace(std::make_pair(f.warp,f.ordinal),f).second,"SiLU typed unique formula");
        }
        std::array<U,32> ordinals{};
        for(const auto& memory:source.program.bodies.at(0).records) {
            p::need(memory.warp>=0&&memory.warp<32,"SiLU typed source warp");
            formulas_.push_back(by_key.at({U(memory.warp),ordinals[memory.warp]++}));
        }
        p::need(formulas_.size()==by_key.size()&&formulas_.size()==744,"SiLU typed complete source formula bijection");
    }
    void finish() const {for(const auto& pin:pins_)(void)check_pin(pin);}
    const J& identity() const {return pins_;}
};

struct TypedParameter {std::string bytes,sha256;};
struct TypedObjectView {
    U pointer=0,bytes=0,root_base=0,root_bytes=0,storage_offset=0,pid=0,start_ticks=0;
    std::string root_id,evidence_sha256;
};
struct TypedCall {
    U pid=0,start_ticks=0,context_id=0,stream=0,native_launch_id=0,epoch=0,ordinal=0;
    std::string source_launch_key,phase,code_sha256,parameter_layout_sha256;
    std::string argument_record_sha256,capture_receipt_sha256;
    std::array<U,3> grid{{0,0,0}},block{{0,0,0}};
    U registers=0,static_shared=0,dynamic_shared=0,local_bytes=0;
    std::array<TypedParameter,3> parameters; // out:u64, input:u64, d:u32, little endian
    TypedObjectView input,out;
};

// Modeled address candidate, intentionally below full KernelBinding admission.
// It owns compiled ranges and its receipt; the source lease may then be released.
class TypedBinding {
    PreparedMemory memory_;
    J evidence_;
    static U decode(const TypedParameter& p0,U size) {
        p::need(p0.bytes.size()==size&&native_typed::hash_text(p0.sha256)&&tiny_sha::sha256(p0.bytes)==p0.sha256,
            "SiLU typed raw argument size/SHA");
        U result=0;for(U i=0;i<size;++i)result|=U(static_cast<unsigned char>(p0.bytes[i]))<<(8*i);return result;
    }
    static PreparedMemory::AddressView validate(const TypedSourceLease& source,const TypedCall& call) {
        p::need(call.pid&&call.start_ticks&&call.context_id&&call.stream==0&&call.native_launch_id,
            "SiLU typed explicit current process/launch/default stream");
        p::need(call.epoch>=2&&call.epoch<=33&&call.phase=="Decode"+std::to_string(call.epoch-1)&&
            call.source_launch_key=="epoch-"+std::to_string(call.epoch)+"-launch-"+std::to_string(call.ordinal),
            "SiLU typed actual Decode phase/ordinal identity");
        p::need(native_typed::hash_text(call.argument_record_sha256)&&native_typed::hash_text(call.capture_receipt_sha256),"SiLU typed explicit capture evidence identity");
        p::need(call.code_sha256==source.source_.input.at("register_program").at("code_sha256").get<std::string>()&&
            call.parameter_layout_sha256==tiny_sha::sha256("[8,8,4]"),"SiLU typed code and ABI decoder binding");
        const auto& resources=source.source_.input.at("register_program").at("native_resources");
        p::need(call.grid==std::array<U,3>{{1,1,1}}&&call.block==std::array<U,3>{{1024,1,1}}&&
            call.registers==p::natural(resources.at("registers"))&&call.local_bytes==p::natural(resources.at("local_bytes_per_thread"))&&
            call.static_shared==0&&call.dynamic_shared==0,"SiLU typed Decode-only native configuration");
        const U out=decode(call.parameters[0],8),input=decode(call.parameters[1],8),d=decode(call.parameters[2],4);
        p::need(d==14336,"SiLU typed fixed BF16 d");
        auto object=[&](const TypedObjectView& view,U pointer,U bytes) {
            p::need(view.pid==call.pid&&view.start_ticks==call.start_ticks&&view.pointer==pointer&&pointer>0&&pointer%16==0&&
                view.bytes==bytes&&!view.root_id.empty()&&native_typed::hash_text(view.evidence_sha256),"SiLU typed same-process actual object view");
            p::need(p::add(view.root_base,view.storage_offset)==pointer&&
                p::add(pointer,bytes)<=p::add(view.root_base,view.root_bytes),"SiLU typed logical root bounds");
            return PreparedMemory::ObjectView{pointer,p::add(pointer,bytes)};
        };
        const auto in=object(call.input,input,57344),output=object(call.out,out,28672);
        p::need(in.end<=output.begin||output.end<=in.begin,"SiLU typed restrict input/output nonalias");
        return {1,in,output};
    }
public:
    TypedBinding(const TypedSourceLease& source,const TypedCall& call)
        :memory_(source.source_.program,source.formulas_,validate(source,call)) {
        evidence_={{"schema","SILU_TYPED_MEMORY_CANDIDATE_V1"},{"qualification","MODELED_ADDRESS_TRANSFER_NOT_NATIVE_ADMISSION"},
            {"source_lease",source.identity()},{"actual_capture",{{"pid",call.pid},{"start_ticks",call.start_ticks},
                {"context_id",call.context_id},{"stream_u64",call.stream},{"native_launch_id",call.native_launch_id},
                {"source_launch_key",call.source_launch_key},{"phase",call.phase},{"code_sha256",call.code_sha256},
                {"parameter_layout_sha256",call.parameter_layout_sha256},{"argument_record_sha256",call.argument_record_sha256},
                {"capture_receipt_sha256",call.capture_receipt_sha256}}},
            {"objects",{{"input",{{"pointer",call.input.pointer},{"bytes",call.input.bytes},{"root_id",call.input.root_id},
                {"evidence_sha256",call.input.evidence_sha256}}},{"out",{{"pointer",call.out.pointer},{"bytes",call.out.bytes},
                {"root_id",call.out.root_id},{"evidence_sha256",call.out.evidence_sha256}}}}},
            {"CTAs",1},{"memory_records_per_CTA",744},{"raw_argument_bytes_SHA_verified",true},
            {"capture_and_root_evidence_identities_supplied_not_reverified",true},{"raw_argument_journal_reverified",false},
            {"dynamic_memory_witness",false},{"native_model_admitted",false},{"compute_dependencies_transferred",false},
            {"native_hardware_timing_qualified",false},{"materializer","shared canonical_silu::PreparedMemory"}};
    }
    const PreparedMemory& memory() const {return memory_;}
    const J& evidence() const {return evidence_;}
};
} // namespace canonical_silu
