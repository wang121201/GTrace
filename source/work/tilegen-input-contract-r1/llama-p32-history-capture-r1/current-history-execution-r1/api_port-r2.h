#pragma once
// Explicit shared-project approximation: L2 coherent, 128B clipped DMA chunks.
// Not a measured Ada copy-engine model. No SM instruction/CTA work is invented.
namespace current_history {
using U=std::uint64_t;using J=nlohmann::json;namespace g=GTSim;
inline void need(bool x,const char*m){if(!x)throw std::runtime_error(m);}
class ApiPort {
 g::L2Cache&cache_;U&now_;std::function<std::vector<int>(U)>advance_;
 const unsigned capacity_;U api_start_=0;std::function<void(const J&)>enter_;std::function<void()>leave_;std::set<int>live_;int next_=1;U accepted_=0,completed_=0,peak_=0;
 void tick(){need(now_>=api_start_&&now_-api_start_<2000000000ULL,"API local modeled cycle bound");need(now_<U(std::numeric_limits<g::Cycle>::max()),"absolute signed cycle overflow");++now_;for(int id:advance_(now_)){need(live_.erase(id)==1,"API owns each returned token exactly once");++completed_;}cache_.end_cycle(g::checked_cycle(now_));}
 void drain(){while(!live_.empty()||!cache_.is_quiescent())tick();}
 void range(const J&effect){
  const bool write=effect.at("operation")=="WRITE";
  need(write||effect.at("operation")=="READ","API operation");
  U at=effect.at("address"),bytes=effect.at("bytes");
  need(at>0&&bytes<=UINT64_MAX-at,"API source extent overflow");
  while(bytes){
   while(live_.size()>=capacity_)tick();
   U size=std::min<U>(bytes,128-at%128),line=at/128*128;
   need(next_<INT_MAX,"API token namespace bound");
   g::ExplicitMemorySubop sub;sub.requested_bytes=size;sub.ranges.push_back({0,at,size});sub.source_member_ordinals.push_back(0);
   g::L2LineRequest request;request.completion_token=next_;request.native_key={1,line};request.cache_key={1,line};request.is_write=write;
   request.sm_id=0;request.subpartition_id=0;request.bypass_l1=true;request.source_matrix_id=1;request.source_subop_index=0;
   request.source_subops=std::span<const g::ExplicitMemorySubop>(&sub,1);
   for(;;){request.cycle=g::checked_cycle(now_);if(cache_.enqueue_line_request(request))break;tick();}
   need(live_.insert(next_++).second,"API unique live completion token");++accepted_;peak_=std::max<U>(peak_,live_.size());at+=size;bytes-=size;
  }
  // D2D all source reads finish before any destination writes in this model.
  // No independent host-return timestamp is treated as cache completion.
  drain();
 }
public:
 ApiPort(g::L2Cache&cache,U&cycle,std::function<std::vector<int>(U)>step,std::function<void(const J&)>enter,std::function<void()>leave,unsigned capacity=32):cache_(cache),now_(cycle),advance_(std::move(step)),capacity_(capacity),enter_(std::move(enter)),leave_(std::move(leave)){need(capacity>0&&capacity<=4096&&bool(enter_)&&bool(leave_),"finite DMA cap and explicit source identity hooks required");}
 J run(const J&api){
  need(cache_.is_quiescent()&&live_.empty(),"API requires preceding kernel/backend completion");
  need(api.at("kind")=="memory_api_submission"&&api.at("API_return_is_not_device_completion")==true,"actual API descriptor required");
  const auto&effects=api.at("device_effect_ranges");std::string direction=api.at("direction");
  J ops=J::array();for(const auto&e:effects)ops.push_back(e.at("operation"));
  if(direction=="device_to_device"){need(ops==J::array({"READ","WRITE"}),"D2D ordered effects");need(effects[0].at("bytes")==effects[1].at("bytes"),"D2D equal extents");}
  else if(direction=="device_to_host")need(ops==J::array({"READ"}),"D2H source effect");
  else{need(direction=="host_to_device"||direction=="device_fill","supported API direction");need(ops==J::array({"WRITE"}),"H2D/fill destination effect");}
  U first=now_,a=accepted_,c=completed_;api_start_=now_;enter_(api);struct Scope{std::function<void()>&leave;~Scope(){leave();}}scope{leave_};
  for(const auto&e:effects)range(e);
  need(accepted_-a==completed_-c&&live_.empty()&&cache_.is_quiescent(),"all actual API cache completions delivered");
  return {{"memory_operation_id",api.at("memory_operation_id")},{"source_effects_completed",effects},{"start_cycle",first},{"quiescent_end_cycle",now_},{"accepted_line_requests",accepted_-a},{"completed_line_requests",completed_-c},{"peak_live_cumulative",peak_},{"admission_cap",capacity_},{"cache_flush",false},{"DMA_hardware_qualified",false},{"DMA_model","L2_COHERENT_128B_CHUNKS_FINITE32_SOURCE_READS_BEFORE_DESTINATION_WRITES"},{"source_SM_identity_present",false}};
 }
};
}
