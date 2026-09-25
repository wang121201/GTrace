#pragma once
// Fixed old-D1 observation only. Never consulted by cache/admission/service.
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>

namespace decode_gate_joint {
using U=std::uint64_t;
inline void need(bool b,const char*s){if(!b)throw std::logic_error(s);}
inline constexpr U kBase=133689123543040ULL,kBytes=57344,kSectors=1792;
inline constexpr int kFirst=408,kLast=772,kGateFirst=425,kGateStride=11,kGates=32;
// Original closed full-D1 total dirty evictions. Candidate qualification must
// preserve it; a subset cannot exceed it. Overflow is failure, never truncation.
inline constexpr U kMaxEvictionEvents=174002;
inline constexpr const char* kPatternSha="3050f286a4027390d68d95cf44ed27b9c52f644c0b78f1e8816c2cb16b514e91";
inline int gate_layer(int call){int d=call-kGateFirst;return d>=0&&d%kGateStride==0&&d/kGateStride<kGates?d/kGateStride:-1;}
struct Sector {
 bool dirty=false,inherited=false,mixed=false,checked=false;
 int first=-1,last=-1,last_touch_call=-1,last_gate_layer=-1;
 U dirty_evictions=0,at_last_gate_touch_evictions=0;
};
struct Call {
 U stores=0,touches=0,unique=0,first_dirty=0,created=0,self_redirty=0,evicted_trigger=0;
 // At this Gate's first touch, compare the previous adjacent Gate's LAST
 // actual store touch of this sector with dirty evictions since that touch.
 U previous_gate_missing=0,adjacent_pairs=0,intervening_evictions=0;
 std::array<U,4> adjacent_joint{}; // index = previous_evicted*2 + current_first_dirty
 template<class J>J json(int call)const{return {{"call_index",call},{"gate_layer",gate_layer(call)},
  {"processed_store_requests",stores},{"store_sector_touches",touches},{"unique_touched_sectors",unique},
  {"first_touch_already_dirty_sectors",first_dirty},{"created_sectors",created},
  {"same_call_touched_then_created_events",self_redirty},{"evicted_trigger_sectors",evicted_trigger},
  {"previous_adjacent_gate_touch_missing_sectors",previous_gate_missing},{"adjacent_gate_sector_pairs",adjacent_pairs},
  {"intervening_dirty_evictions_sum",intervening_evictions},{"adjacent_evicted_first_dirty_bins",adjacent_joint}};}
};
struct Bin {U sectors=0,mixed=0;};
class Observation {
 std::array<Sector,kSectors> sectors_{};
 std::array<Call,kLast-kFirst+1> calls_{};
 std::map<std::array<int,3>,Bin> evicted_joint_;
 std::map<std::array<int,2>,Bin> final_joint_;
 int call_=-1;U initial_=0,created_=0,evicted_=0,final_=0,touches_=0,stores_=0;
 bool begun_=false,checking_=false,closed_=false,abandoned_=false;
 void active()const{need(begun_&&!closed_&&!checking_&&!abandoned_&&call_>=kFirst,"gate joint outside actual call");}
 static bool selected(int matrix,U address){return matrix==1&&address>=kBase&&address<kBase+kBytes;}
 Sector& at(U address){need(address%32==0&&address>=kBase&&address<kBase+kBytes,"gate joint address domain");return sectors_.at((address-kBase)/32);}
 Call& row(){return calls_.at(call_-kFirst);}
public:
 Observation()=default;
 Observation(const Observation&)=delete;Observation&operator=(const Observation&)=delete;
 Observation(Observation&&)=delete;Observation&operator=(Observation&&)=delete;
 void begin(int first,int last){need(!begun_&&!closed_&&!abandoned_&&first==kFirst&&last==kLast,"gate joint fixed natural D1 scope");begun_=true;}
 void seed(int matrix,U line,unsigned mask){
  need(begun_&&call_==-1&&!closed_&&!abandoned_,"gate joint seed outside entry");
  for(unsigned bit=0;bit<4;++bit)if((mask&(1u<<bit))&&selected(matrix,line+32*bit)){
   auto&s=at(line+32*bit);need(!s.dirty,"gate joint duplicate inherited sector");s.dirty=s.inherited=true;++initial_;
  }
 }
 void set_call(int call){need(begun_&&!closed_&&!checking_&&!abandoned_&&call==(call_==-1?kFirst:call_+1)&&call<=kLast,"gate joint contiguous calls required");call_=call;}
 void store(int matrix,U line,unsigned before,unsigned mask){
  active();bool any=false;
  for(unsigned bit=0;bit<4;++bit)if((mask&(1u<<bit))&&selected(matrix,line+32*bit)){
   any=true;auto&s=at(line+32*bit);auto&r=row();const bool was_dirty=before&(1u<<bit);
   need(s.dirty==was_dirty,"gate joint/native prior dirty state mismatch");
   const bool first_touch=s.last_touch_call!=call_;
   ++touches_;++r.touches;
   if(first_touch){++r.unique;r.first_dirty+=was_dirty;}
   if(!was_dirty){++created_;++r.created;if(!first_touch)++r.self_redirty;s.first=s.last=call_;s.inherited=s.mixed=false;s.dirty=true;}
   else{if(s.last>=0&&s.last!=call_)s.mixed=true;s.last=call_;}
   const int layer=gate_layer(call_);
   if(layer>=0){
    if(first_touch){
     if(layer>0&&s.last_gate_layer==layer-1){
      need(s.dirty_evictions>=s.at_last_gate_touch_evictions,"gate joint eviction counter reversed");
      U intervening=s.dirty_evictions-s.at_last_gate_touch_evictions;
      ++r.adjacent_pairs;r.intervening_evictions+=intervening;
      ++r.adjacent_joint[(intervening>0?2:0)+(was_dirty?1:0)];
      need(intervening>0||was_dirty,"prior Gate dirty obligation disappeared without observed eviction");
     }else ++r.previous_gate_missing;
    }
    s.last_gate_layer=layer;s.at_last_gate_touch_evictions=s.dirty_evictions;
   }
   s.last_touch_call=call_;
  }
  if(any){++stores_;++row().stores;}
 }
 void evict_sector(int matrix,U address,int first,int last,bool mixed,bool inherited){
  active();if(!selected(matrix,address))return;
  auto&s=at(address);
  need(s.dirty&&s.first==first&&s.last==last&&s.mixed==mixed&&s.inherited==inherited,"gate joint/old writer victim labels disagree");
  need((first==-1)==inherited&&first<=last&&last<=call_,"gate joint victim time/unknown identity");
  need(evicted_<kMaxEvictionEvents,"gate joint bounded eviction count exceeded");
  const std::array<int,3> key{first,last,call_};auto it=evicted_joint_.find(key);
  if(it==evicted_joint_.end()){need(evicted_joint_.size()<kMaxEvictionEvents,"gate joint bin bound exceeded");it=evicted_joint_.emplace(key,Bin{}).first;}
  ++it->second.sectors;it->second.mixed+=mixed;
  ++evicted_;++row().evicted_trigger;++s.dirty_evictions;
  s.dirty=s.inherited=s.mixed=false;s.first=s.last=-1;
 }
 void check_begin(){active();need(call_==kLast,"gate joint incomplete D1 window");checking_=true;for(auto&s:sectors_)s.checked=false;}
 void check_line(int matrix,U line,unsigned mask){
  need(checking_&&!closed_&&!abandoned_,"gate joint final check state");
  for(unsigned bit=0;bit<4;++bit)if((mask&(1u<<bit))&&selected(matrix,line+32*bit)){
   auto&s=at(line+32*bit);need(s.dirty&&!s.checked,"gate joint final native dirty mismatch");s.checked=true;
  }
 }
 void end(){
  need(checking_&&!closed_&&!abandoned_,"gate joint end state");
  std::array<U,kLast-kFirst+1> first_total{};U unknown=0,bin_e=0;
  for(const auto&[key,v]:evicted_joint_){bin_e+=v.sectors;if(key[0]<0)unknown+=v.sectors;else first_total.at(key[0]-kFirst)+=v.sectors;}
  need(bin_e==evicted_,"gate joint eviction bin closure");
  for(const auto&s:sectors_){need(s.dirty==s.checked,"gate joint final dirty domain closure");if(!s.dirty)continue;
   ++final_;auto&v=final_joint_[{s.first,s.last}];++v.sectors;v.mixed+=s.mixed;
   if(s.first<0)unknown++;else first_total.at(s.first-kFirst)++;
  }
  need(initial_+created_==evicted_+final_&&unknown==initial_,"gate joint lifetime/unknown conservation");
  U c=0,t=0,p=0,e=0;for(std::size_t i=0;i<calls_.size();++i){const auto&r=calls_[i];
   need(r.created==r.unique-r.first_dirty+r.self_redirty&&r.first_dirty<=r.unique,"gate joint exact intra-call creation decomposition");
   need(first_total[i]==r.created,"gate joint first writer creation closure");
   if(gate_layer(int(i)+kFirst)>=0){U bins=0;for(U b:r.adjacent_joint)bins+=b;need(bins==r.adjacent_pairs&&r.adjacent_pairs+r.previous_gate_missing==r.unique,"gate joint adjacent Gate population closure");}
   else need(r.adjacent_pairs==0&&r.previous_gate_missing==0&&r.intervening_evictions==0,"nonGate adjacent population invalid");
   c+=r.created;t+=r.touches;p+=r.stores;e+=r.evicted_trigger;
  }
  need(c==created_&&t==touches_&&p==stores_&&e==evicted_,"gate joint call counters closure");closed_=true;checking_=false;
 }
 void abandon(){if(!closed_)abandoned_=true;}
 template<class J>J report()const{
  need(closed_&&!abandoned_,"gate joint report before closed observation");
  J calls=J::array(),gates=J::array(),evictions=J::array(),finals=J::array();
  for(std::size_t i=0;i<calls_.size();++i)calls.push_back(calls_[i].template json<J>(int(i)+kFirst));
  for(int i=0;i<kGates;++i)gates.push_back(kGateFirst+kGateStride*i);
  for(const auto&[k,v]:evicted_joint_)evictions.push_back({{"first_store_call",k[0]},{"last_store_call",k[1]},{"trigger_call",k[2]},{"sectors",v.sectors},{"mixed_observed_calls_sectors",v.mixed}});
  for(const auto&[k,v]:final_joint_)finals.push_back({{"first_store_call",k[0]},{"last_store_call",k[1]},{"sectors",v.sectors},{"mixed_observed_calls_sectors",v.mixed}});
  return {{"schema","FIXED_D1_GATE_DIRTY_JOINT_V1"},{"status","PASS_CLOSED_FIXED_GATE_JOINT"},
   {"first_call",kFirst},{"last_call",kLast},{"matrix",1},{"range_base",kBase},{"range_bytes",kBytes},{"range_sectors",kSectors},
   {"gate_calls",gates},{"source_pattern_sha256",kPatternSha},{"initial_inherited_dirty_sectors",initial_},
   {"created_dirty_sectors",created_},{"evicted_dirty_sectors",evicted_},{"final_dirty_sectors",final_},
   {"processed_store_requests",stores_},{"store_sector_touches",touches_},{"writeback_bytes",evicted_*32},
   {"calls",calls},{"eviction_joint",evictions},{"final_joint",finals},{"eviction_event_bound",kMaxEvictionEvents},
   {"fixed_sector_state_entries",kSectors},{"overflow",false},{"model_behavior_changed",false},{"complete_event_trace",false},
   {"scope","fixed canonical old P32 Decode1 Gate VA diagnostic, not a cache policy"},
   {"unknown_call",-1},{"adjacent_bin_order","2*previous_dirty_eviction_since_previous_Gate_last_touch + current_first_touch_already_dirty"},
   {"same_call_repeat_semantics","a created dirty sector already touched by an actual store in this same call"},
   {"adjacent_semantics","current Gate first touch compared with previous adjacent Gate last touch of the same sector; intermediate nonGate stores allowed"},
   {"writer_semantics","actual processed store call touching any bytes; neither tensor owner nor byte provenance"}};
 }
};
} // namespace decode_gate_joint
