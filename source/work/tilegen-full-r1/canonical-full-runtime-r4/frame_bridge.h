#pragma once
#include <functional>
#include <sstream>
namespace frame_bridge {
using J=nlohmann::json;using U=std::uint64_t;
void need(bool v,const std::string&s){if(!v)throw std::runtime_error("Frame bridge: "+s);}
U natural(const J&j){need(j.is_number_integer()&&!j.is_boolean(),"integer field");need(j.is_number_unsigned()||j.get<std::int64_t>()>=0,"nonnegative field");return j.get<U>();}
J parse(const std::string&s){std::vector<std::set<std::string>>stack;return J::parse(s,[&](int,J::parse_event_t e,J&v){if(e==J::parse_event_t::object_start)stack.emplace_back();if(e==J::parse_event_t::key)need(stack.back().insert(v.get<std::string>()).second,"duplicate JSON key");if(e==J::parse_event_t::object_end)stack.pop_back();return true;});}
std::string line(std::istream&in,std::size_t cap){std::string s;for(;;){char c;need(bool(in.get(c)),"truncated header");if(c=='\n')return s;need(s.size()<cap,"header cap");s.push_back(c);}}
struct RawCache {
 const U frame_cap,total_cap;std::map<std::string,J>expected;std::map<std::string,std::string>raw;U bytes=0,ingestions=0;
 RawCache(const J&manifest,U per=64ULL<<20,U total=128ULL<<20):frame_cap(per),total_cap(total){need(manifest.is_array()&&!manifest.empty()&&manifest.size()<=8,"one through eight frames");U sum=0;for(const auto&q:manifest){need(q.is_object()&&q.size()==3&&q.contains("key")&&q.contains("bytes")&&q.contains("sha256"),"frame declaration fields");auto k=q.at("key").get<std::string>();U n=natural(q.at("bytes"));need(!k.empty()&&k.size()<=64&&n&&n<=frame_cap,"frame declaration bound");need(q.at("sha256").is_string()&&q.at("sha256").get<std::string>().size()==64,"SHA field");need(expected.emplace(k,q).second,"duplicate frame declaration");need(n<=total_cap-sum,"aggregate cache cap");sum+=n;}}
 void read_one(std::istream&in){auto h=parse(line(in,4096));need(h.is_object()&&h.size()==3&&h.contains("key")&&h.contains("bytes")&&h.contains("sha256"),"frame header fields");auto key=h.at("key").get<std::string>();need(expected.count(key)&&!raw.count(key),"unknown or duplicate frame");need(h==expected.at(key)&&natural(h.at("bytes"))==natural(expected.at(key).at("bytes")),"exact expected header");U n=natural(h.at("bytes"));need(n<=total_cap-bytes,"aggregate cache cap");std::string payload;payload.resize(n);U offset=0;while(offset<n){U take=std::min<U>(65536,n-offset);in.read(&payload[std::size_t(offset)],std::streamsize(take));need(U(in.gcount())==take,"truncated payload");offset+=take;}need(tiny_sha::sha256(payload)==h.at("sha256").get<std::string>(),"payload SHA");raw.emplace(key,std::move(payload));bytes+=n;++ingestions;}
 void finish(std::istream&in){need(raw.size()==expected.size(),"missing declared frame");need(in.peek()==std::char_traits<char>::eof(),"trailing transport bytes");}
 const std::string&get(const std::string&k)const{auto i=raw.find(k);need(i!=raw.end(),"uncached template");return i->second;}
};
// Every factory must capture a Lease, not a bare Model reference. The session is
// externally owned; this class cannot create/reset/finalize it or its cache.
template<class Model>class Active {
 std::shared_ptr<const Model>owner_;std::string key_;std::function<bool()>quiescent_;U live_graph_scopes_=0;bool call_open_=false;
public:
 U constructions=0;
 using Lease=std::shared_ptr<const Model>;
 explicit Active(std::function<bool()>quiescent):quiescent_(std::move(quiescent)){}
 template<class Loader>void select(const std::string&key,Loader load){need(quiescent_(),"session not quiescent");need(!call_open_&&live_graph_scopes_==0,"call/graph scope still live");need(!owner_||owner_.use_count()==1,"borrowed factory/model still live");if(owner_&&key==key_)return;owner_.reset();key_.clear();auto next=load();need(bool(next),"null typed model");owner_=std::move(next);key_=key;++constructions;}
 Lease lease()const{need(bool(owner_),"no active model");return owner_;}
 void begin_call(){need(!call_open_&&quiescent_()&&bool(owner_),"call begin boundary");call_open_=true;}
 void end_call(){need(call_open_&&quiescent_()&&live_graph_scopes_==0,"call end boundary");call_open_=false;}
 void graph_enter(){need(call_open_,"graph requires call");++live_graph_scopes_;}
 void graph_leave(){need(live_graph_scopes_>0,"graph counter underflow");--live_graph_scopes_;}
};
}
