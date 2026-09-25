#pragma once
// Included after the immutable production p28::Model and p28::Builder.
// Current bindings never enter or weaken the original old-PID target validator.
namespace current_gemm {
using J=nlohmann::json;using U=std::uint64_t;namespace g=GTSim;
inline U checks=0;
inline void need(bool ok,const std::string& m){++checks;if(!ok)throw std::runtime_error("Current GEMM Fine bridge: "+m);}
inline U num(const J& j){need(j.is_number_integer()&&!j.is_boolean()&&(j.is_number_unsigned()||j.get<std::int64_t>()>=0),"nonnegative integer");return j.get<U>();}
inline std::string unhex(const std::string& s){need(s.size()==384,"actual192 ABI bytes");std::string out;for(std::size_t i=0;i<s.size();i+=2){auto digit=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("noncanonical hex");};out.push_back(char((digit(s[i])<<4)|digit(s[i+1])));}return out;}
inline U word(const std::string& raw,int offset){U v=0;for(int i=0;i<8;++i)v|=U(static_cast<unsigned char>(raw.at(offset+i)))<<(8*i);return v;}
inline void validate_call(const J& entry,const J& frames){
 const J& b=entry.at("binding");std::string role=b.at("role_name"),key=entry.at("frame_key");const auto& f=frames.at(key);
 const std::map<std::string,std::string> family{{"qkv","P28QKV"},{"o","GEMMO"},{"gate","GEMMGate"},{"down","GEMMDown"}};
 need(family.at(role)==key&&b.at("schema")=="CURRENT_P32_NATIVE_GEMM_ADDRESS_BINDING_V1","explicit current role schema");
 need(b.at("process")==J({{"pid",3011726},{"start_ticks",897090720}})&&b.at("native_launch_binding").at("process")==b.at("process"),"current process");
 need((b.at("phase")=="Warmup/Prefill"||b.at("phase")=="Measured/Prefill")&&num(b.at("layer"))<32,"finite P32 stage/layer");
 need(b.at("grid")==J::array({f.at("full_grid"),1,1})&&b.at("block")==J::array({128,1,1}),"source geometry");
 need(b.at("context_id")==1&&b.at("stream_u64")==0&&num(entry.at("current_native_before_event"))<num(entry.at("current_native_return_event")),"current source callback order/domain");
 auto raw=unhex(entry.at("raw_argument_bytes_hex"));need(tiny_sha::sha256(raw)==b.at("argument_raw_sha256").get<std::string>(),"actual raw SHA");std::string np,z=raw;
 for(int i=0;i<192;++i){bool pointer=i<24||(i>=168&&i<176);if(pointer)z[i]=0;else np.push_back(raw[i]);}
 need(np.size()==160&&tiny_sha::sha256(np)==b.at("nonpointer_sha256").get<std::string>()&&tiny_sha::sha256(z)==b.at("pointer_zeroed_sha256").get<std::string>()&&b.at("pointer_zeroed_sha256")==b.at("source_pointer_zeroed_sha256"),"all160 exact nonpointer bytes");
 const std::array<std::string,3>roles{{"weight","input","output"}};
 for(int ri=0;ri<3;++ri){const auto&r=roles[ri];const J&o=b.at("objects").at(r),v=o.at("view"),root=o.at("root"),source=f.at("objects")[ri];U p=num(o.at("pointer")),sz=num(o.at("bytes"));
  need(word(raw,ri*8)==p&&b.at("pointers").at(r)==p,"raw actual role pointer");need(source.at("role")==r&&source.at("bytes")==sz&&p%128==0,"frame role/full extent");need(o.at("end_exclusive")==p28::add(p,sz)&&num(root.at("base_address"))+num(v.at("storage_offset_bytes"))==p&&p28::add(p,sz)<=p28::add(num(root.at("base_address")),num(root.at("storage_nbytes"))),"current typed root span");
  need(v.at("data_address")==p&&v.at("dtype")=="torch.bfloat16"&&v.at("element_size")==2&&v.at("logical_nbytes")==sz&&o.at("shape")==v.at("shape")&&o.at("stride_bytes")==v.at("stride_bytes"),"typed source format");
  U bytes=2,span=2;need(v.at("shape").size()==v.at("stride_bytes").size(),"rank");for(std::size_t i=0;i<v.at("shape").size();++i){U n=num(v.at("shape")[i]);need(n>0,"nonempty tensor");bytes=p28::mul(bytes,n);span=p28::add(span,p28::mul(n-1,num(v.at("stride_bytes")[i])));}need(bytes==sz&&span==sz&&num(f.at("full_grid_last_byte_exclusive").at(r))<=sz,"all-grid native role coverage");
 }
 need(word(raw,168)==word(raw,16),"C/Y output alias retained");
 for(int a=0;a<3;++a)for(int c=a+1;c<3;++c){const auto&x=b.at("objects").at(roles[a]);const auto&y=b.at("objects").at(roles[c]);need(num(x.at("end_exclusive"))<=num(y.at("pointer"))||num(y.at("end_exclusive"))<=num(x.at("pointer")),"disjoint typed roles");}
 need(b.at("current_occupancy").is_null(),"unknown occupancy retained");for(const std::string k:{"current_occupancy_qualified","native_target_qualified","compute_transfer_qualified","implicit_register_dependencies_complete","allocator_lifetime_qualified","full_cpp_parameter_struct_proven","full_program_qualified","full_simulation_admitted"})need(b.at(k)==false,"lower qualification "+k);
}
class PreparedMemory {
 J target_,envelope_;std::unique_ptr<p28::Model> model_;
public:
 PreparedMemory(const J& entry,const J& frames,const J& original):target_(entry.at("binding")),envelope_(original){
  validate_call(entry,frames);need(entry.at("frame_key")=="P28QKV","bounded executable role QKV; other roles remain explicit plan-only");const auto& proof=frames.at("P28QKV");
  need(tiny_sha::sha256(original.at("nodes").dump())==proof.at("nodes_sha256").get<std::string>()&&original.at("nodes").size()==num(proof.at("node_count")),"unaltered frozen complete source DAG");
  envelope_["ctas"]=proof.at("full_grid");for(auto&o:envelope_["objects"]){const auto&now=target_.at("objects").at(o.at("role").get<std::string>());o["pointer"]=now.at("pointer");o["logical_identity"]=now.at("logical_identity");}
  J inverse=envelope_;inverse["ctas"]=original.at("ctas");inverse["objects"]=original.at("objects");need(inverse==original,"only checked current object identity and declared CTA domain rebound");
  model_=std::make_unique<p28::Model>(envelope_);need(model_->nodes.size()==39404&&model_->copies==2576&&model_->zero_copies==16,"original typed model census");
 }
 const p28::Model& model()const{return *model_;}
 const J& source_node(std::size_t i)const{return envelope_.at("nodes").at(i);}
 const J& target()const{return target_;}
 // Actual Fine lowering; this does not flatten copies or invent a scheduler.
 g::CtaGraphStore::Owned build_cta(int c){need(c==0||c==47,"only requested first/last current QKV CTA");p28::Builder builder(*model_);auto nodes=builder.build(c);need(nodes.size()==39404,"complete actual Fine DAG");return nodes;}
};
inline J subop(const g::ExplicitMemorySubop&s){J rs=J::array();for(const auto&r:s.ranges)rs.push_back({r.source_member_ordinal,r.offset_bytes,r.byte_count});return {{"requested_bytes",s.requested_bytes},{"ranges",rs},{"source_member_ordinals",s.source_member_ordinals}};}
inline J compare_event(const PreparedMemory& prepared,int c,std::size_t i,const g::DAGNode& n,const J& expected){
 const auto&t=prepared.model().nodes.at(i);const auto&source=prepared.source_node(i);need(t.kind=="global"||t.kind=="async_copy","memory source kind");need(expected.at("cta_linear_id")==c&&expected.at("cta_warp_id")==t.warp&&expected.at("memory_ordinal")==source.at("memory_record_ordinal"),"original memory occurrence identity");
 need(expected.at("pc")==t.pc&&expected.at("opcode")==t.opcode&&expected.at("width")==t.width&&expected.at("instruction_effective_mask")==t.effective,"PC/width/instruction mask");
 need(n.explicit_memory_subops.size()==1&&n.memory_access_granularity_bytes==int(t.width)&&n.memory_coalesce_bytes==128,"actual Fine explicit source contract");J actual=J::object();U mask=0,bytes=0;for(const auto&r:n.explicit_memory_subops[0].ranges){actual[std::to_string(r.source_member_ordinal)]=r.offset_bytes;mask|=U(1)<<r.source_member_ordinal;bytes+=r.byte_count;need(r.byte_count==t.width,"full lane width");}
 need(actual==expected.at("lane_addresses")&&mask==num(expected.at("global_effective_mask"))&&bytes==n.explicit_memory_subops[0].requested_bytes,"all current source addresses/multiplicity");
 if(t.kind=="async_copy"){
  need(expected.at("operation")=="GLOBAL_TO_SHARED"&&n.op_type==g::OpType::CP_DRAM2SRAM_LDGSTS&&n.explicit_async_shared_service_v1&&n.async_copy_bypass_l1,"actual copy remains G2S, never ordinary load");
  need(mask==t.source_mask&&expected.at("source_read_mask")==mask&&expected.at("program_ordinal")==t.ordinal&&expected.at("transfer_width")==t.width,"independent read mask and program occurrence");
  need(n.async_copy_shared_subops.size()==1&&expected.at("source_zero_copy_semantics_qualified")==false,"shared destination and qualification");const auto&dst=n.async_copy_shared_subops[0];need(dst.ranges.size()==std::size_t(__builtin_popcount(unsigned(t.effective))),"shared instruction lanes include source-unread lanes");
  for(const auto&r:dst.ranges)need(expected.at("shared_addresses").at(r.source_member_ordinal)==r.offset_bytes&&r.byte_count==16,"shared destination/width exact");
 }else need(expected.at("operation")=="WRITE"&&n.op_type==g::OpType::ST_REG2DRAM&&mask==t.effective,"native output store");
 return {{"cta",c},{"warp",t.warp},{"memory_ordinal",source.at("memory_record_ordinal")},{"pc",t.pc},{"opcode",t.opcode},{"width",t.width},{"instruction_mask",t.effective},{"source_mask",mask},{"global",subop(n.explicit_memory_subops[0])},{"shared",t.kind=="async_copy"?subop(n.async_copy_shared_subops[0]):J(nullptr)}};
}
}
