#pragma once
// Passive, window-local attribution. No VA history survives an eviction.
#include <array>
#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "gate_joint_observer.h"

namespace decode_writer {
using U=std::uint64_t;
inline void need(bool b,const char* s){if(!b)throw std::logic_error(s);}
inline unsigned bits(unsigned x){unsigned n=0;for(unsigned i=0;i<4;++i)n+=(x>>i)&1;return n;}
struct Key {int matrix;U line;bool operator==(const Key&)const=default;};
struct Hash {std::size_t operator()(const Key& k)const{return std::hash<U>{}(k.line)^(std::hash<int>{}(k.matrix)<<1);}};
struct Cell {int first=-1,last=-1;bool mixed=false,inherited=false;};
struct Line {unsigned mask=0;std::array<Cell,4> cells;U checked_epoch=0;};
struct Counts {
 U processed_stores=0,store_sector_touches=0,created=0,already_dirty_touches=0;
 U evicted_by_trigger=0,evicted_first=0,evicted_last=0,final_first=0,final_last=0;
 template<class J>J json()const{return {{"processed_stores",processed_stores},{"store_sector_touches",store_sector_touches},{"created_sectors",created},{"already_dirty_sector_touches",already_dirty_touches},{"evicted_trigger_sectors",evicted_by_trigger},{"evicted_first_store_sectors",evicted_first},{"evicted_last_store_sectors",evicted_last},{"final_first_store_sectors",final_first},{"final_last_store_sectors",final_last}};}
};
struct Native {U stores,created,evicted,write_bytes,completed_write_bytes;};
class Observation {
 int first_,last_,call_=-1;U bound_=0,peak_=0,initial_=0,created_=0,touches_=0,stores_=0,evicted_=0,final_=0;
 U inherited_evicted_=0,inherited_final_=0,unknown_last_evicted_=0,unknown_last_final_=0;
 U mixed_evicted_=0,mixed_final_=0,check_epoch_=0,checked_sectors_=0,checked_lines_=0;
 bool begun_=false,closed_=false,abandoned_=false;
 bool unique_enabled_=false;U unique_written_=0,unique_evicted_=0;
 static constexpr U unique_sector_bound_=2000000;
 std::unordered_map<Key,unsigned,Hash> written_seen_,evicted_seen_;
 decode_gate_joint::Observation* joint_=nullptr;
 Native before_{};std::vector<Counts> calls_;std::unordered_map<Key,Line,Hash> live_;
 Counts& row(int c){need(c>=first_&&c<=last_,"writer call outside fixed window");return calls_.at(c-first_);}
 void key_ok(const Key&k,unsigned mask)const{need(k.matrix>=0&&k.line%128==0&&mask>0&&mask<16,"writer key/mask invalid");}
 void active()const{need(begun_&&!closed_&&!abandoned_&&call_>=first_,"writer outside active call");}
 void room(){need(live_.size()<bound_,"writer live dirty-line bound exceeded");}
 void unique(std::unordered_map<Key,unsigned,Hash>&seen,U&count,const Key&k,unsigned mask){if(!unique_enabled_)return;auto it=seen.find(k);unsigned before=it==seen.end()?0:it->second;U added=bits(mask&~before);need(count+added<=unique_sector_bound_,"writer unique-sector hard bound exceeded");if(it==seen.end())seen.emplace(k,mask);else it->second|=mask;count+=added;}
public:
 explicit Observation(int first,int last,bool unique_history=false,decode_gate_joint::Observation* joint=nullptr,int max_calls=1138):first_(first),last_(last),unique_enabled_(unique_history),joint_(joint){need(max_calls>0&&first>=0&&last>=first&&last-first<max_calls,"writer fixed call domain invalid");calls_.resize(last-first+1);}
 void begin(U bound,Native n){need(!begun_&&!closed_&&!abandoned_&&bound>0&&bound<=400000,"writer begin/bound invalid");bound_=bound;before_=n;if(joint_)joint_->begin(first_,last_);begun_=true;}
 void seed(int matrix,U line,unsigned mask){need(begun_&&call_==-1&&!closed_,"writer seed outside entry");Key k{matrix,line};key_ok(k,mask);room();Line x;x.mask=mask;for(unsigned s=0;s<4;++s)if(mask&(1u<<s))x.cells[s].inherited=true;need(live_.emplace(k,x).second,"writer duplicate inherited line");initial_+=bits(mask);peak_=std::max<U>(peak_,live_.size());if(joint_)joint_->seed(matrix,line,mask);}
 void set_call(int call){need(begun_&&!closed_&&!abandoned_,"writer call before begin/after end");need(call==(call_==-1?first_:call_+1)&&call<=last_,"writer calls must be contiguous");call_=call;if(joint_)joint_->set_call(call);}
 void store(int matrix,U line,unsigned before,unsigned mask){active();Key k{matrix,line};key_ok(k,mask);need(before<16,"writer prior mask invalid");auto it=live_.find(k);need((it==live_.end()?0:it->second.mask)==before,"writer store/native dirty mask mismatch");if(it==live_.end()){room();it=live_.emplace(k,Line{}).first;}auto&x=it->second;if(joint_)joint_->store(matrix,line,before,mask);auto&r=row(call_);++stores_;++r.processed_stores;touches_+=bits(mask);r.store_sector_touches+=bits(mask);
  for(unsigned s=0;s<4;++s)if(mask&(1u<<s)){auto&c=x.cells[s];if(!(before&(1u<<s))){c={call_,call_,false,false};++created_;++r.created;}else{++r.already_dirty_touches;if(c.last>=0&&c.last!=call_)c.mixed=true;c.last=call_;}}
  x.mask=before|mask;unique(written_seen_,unique_written_,k,mask);peak_=std::max<U>(peak_,live_.size());
 }
 void evict(int matrix,U line,unsigned mask){active();Key k{matrix,line};key_ok(k,mask);auto it=live_.find(k);need(it!=live_.end()&&it->second.mask==mask,"writer eviction/native dirty mask mismatch");row(call_).evicted_by_trigger+=bits(mask);evicted_+=bits(mask);
  for(unsigned s=0;s<4;++s)if(mask&(1u<<s)){const auto&c=it->second.cells[s];if(joint_)joint_->evict_sector(matrix,line+32*s,c.first,c.last,c.mixed,c.inherited);if(c.first>=0)++row(c.first).evicted_first;else ++inherited_evicted_;if(c.last>=0)++row(c.last).evicted_last;else ++unknown_last_evicted_;mixed_evicted_+=c.mixed;}
  unique(evicted_seen_,unique_evicted_,k,mask);live_.erase(it);
 }
 void check_begin(){active();need(call_==last_,"writer final call incomplete");++check_epoch_;checked_sectors_=checked_lines_=0;if(joint_)joint_->check_begin();}
 void check_line(int matrix,U line,unsigned mask){Key k{matrix,line};key_ok(k,mask);auto it=live_.find(k);need(it!=live_.end()&&it->second.mask==mask&&it->second.checked_epoch!=check_epoch_,"writer final mask/duplicate mismatch");it->second.checked_epoch=check_epoch_;checked_sectors_+=bits(mask);++checked_lines_;if(joint_)joint_->check_line(matrix,line,mask);}
 void end(Native n){active();need(check_epoch_>0&&checked_lines_==live_.size(),"writer final live domain mismatch");need(n.stores>=before_.stores&&n.created>=before_.created&&n.evicted>=before_.evicted&&n.write_bytes>=before_.write_bytes&&n.completed_write_bytes>=before_.completed_write_bytes,"writer native counters reversed");need(n.stores-before_.stores==stores_&&n.created-before_.created==created_&&n.evicted-before_.evicted==evicted_,"writer/native mutation counters mismatch");need(n.write_bytes-before_.write_bytes==evicted_*32&&n.completed_write_bytes-before_.completed_write_bytes==evicted_*32,"writer/native completed WB bytes mismatch");final_=checked_sectors_;need(initial_+created_==evicted_+final_,"writer dirty stock conservation");
  for(const auto&[k,x]:live_)for(unsigned s=0;s<4;++s)if(x.mask&(1u<<s)){const auto&c=x.cells[s];if(c.first>=0)++row(c.first).final_first;else ++inherited_final_;if(c.last>=0)++row(c.last).final_last;else ++unknown_last_final_;mixed_final_+=c.mixed;}
  U first_e=inherited_evicted_,last_e=unknown_last_evicted_,first_f=inherited_final_,last_f=unknown_last_final_;for(const auto&r:calls_){first_e+=r.evicted_first;last_e+=r.evicted_last;first_f+=r.final_first;last_f+=r.final_last;}need(first_e==evicted_&&last_e==evicted_&&first_f==final_&&last_f==final_,"writer attribution marginal closure");if(joint_)joint_->end();closed_=true;
 }
 void abandon(){if(joint_)joint_->abandon();if(!closed_)abandoned_=true;}
 template<class J>J snapshot()const{J rows=J::array();for(std::size_t i=0;i<calls_.size();++i){auto r=calls_[i].template json<J>();r["call_index"]=first_+i;rows.push_back(r);}return {{"schema","DECODE_STORE_CALL_ATTRIBUTION_V1"},{"status",closed_&&!abandoned_?"PASS_CLOSED_PASSIVE_ATTRIBUTION":"INCOMPLETE"},{"first_call",first_},{"last_call",last_},{"last_observed_call",call_},{"initial_inherited_dirty_sectors",initial_},{"created_dirty_sectors",created_},{"evicted_dirty_sectors",evicted_},{"final_dirty_sectors",closed_?J(final_):J(nullptr)},{"writeback_bytes",evicted_*32},{"processed_stores",stores_},{"store_sector_touches",touches_},{"already_dirty_sector_touches",touches_-created_},{"inherited_first_evicted_sectors",inherited_evicted_},{"inherited_first_final_sectors",inherited_final_},{"unknown_last_evicted_sectors",unknown_last_evicted_},{"unknown_last_final_sectors",unknown_last_final_},{"mixed_observed_calls_evicted_sectors",mixed_evicted_},{"mixed_observed_calls_final_sectors",mixed_final_},{"peak_live_dirty_lines",peak_},{"live_dirty_lines",live_.size()},{"live_line_bound",bound_},{"calls",rows},{"model_behavior_changed",false},{"complete_event_trace",false},{"unique_or_repeat_sector_history_available",false},{"first_semantics","actual store call causing 0-to-1 in this dirty lifetime; entry-inherited is unknown"},{"last_semantics","last actual processed store call touching sector, not byte ownership"},{"mixed_semantics","two or more observed store calls touched sector in one live dirty lifetime; inherited earlier writer remains unknown"}};}
 template<class J>J report()const{need(closed_&&!abandoned_,"writer report before closed end");auto j=snapshot<J>();j["unique_or_repeat_sector_history_available"]=unique_enabled_;j["unique_written_sectors"]=unique_enabled_?J(unique_written_):J(nullptr);j["unique_evicted_sectors"]=unique_enabled_?J(unique_evicted_):J(nullptr);j["repeated_evicted_sector_events"]=unique_enabled_?J(evicted_-unique_evicted_):J(nullptr);j["unique_sector_bound_per_set"]=unique_sector_bound_;j["unique_semantics"]="optional window-only (matrix,VA,32B sector) sets; inherited victims included in eviction set; repeats do not establish redundant logical writes or byte ownership";return j;}
};
inline thread_local Observation* current=nullptr;
inline thread_local const void* owner=nullptr;
class Scope {
 Observation& observation_;
public:
 Scope(Observation&o,const void*p):observation_(o){need(!current&&p,"writer nested/null scope");current=&o;owner=p;}
 ~Scope(){observation_.abandon();current=nullptr;owner=nullptr;}
 Scope(const Scope&)=delete;Scope&operator=(const Scope&)=delete;
};
inline Observation* attached(const void*p){return owner==p?current:nullptr;}
inline void store(const void*p,int m,U line,unsigned before,unsigned mask){if(auto*o=attached(p))o->store(m,line,before,mask);}
inline void evict(const void*p,int m,U line,unsigned mask){if(auto*o=attached(p))o->evict(m,line,mask);}
} // namespace decode_writer
