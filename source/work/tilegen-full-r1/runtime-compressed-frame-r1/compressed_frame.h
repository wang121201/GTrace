#pragma once
#include <zlib.h>
#include <type_traits>
namespace compressed_frame {
using J=nlohmann::json;using U=std::uint64_t;
constexpr U CAP=128ULL<<20;
inline void need(bool x,const std::string&s){if(!x)throw std::runtime_error("Compressed RAM frame: "+s);}
inline U natural(const J&v){need(v.is_number_integer()&&!v.is_boolean(),"integer length");need(v.is_number_unsigned()||v.get<std::int64_t>()>=0,"nonnegative length");return v.get<U>();}
inline bool sha_field(const J&v){if(!v.is_string())return false;const auto&s=v.get_ref<const std::string&>();return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
inline void declaration(const J&q){
 need(q.is_object()&&q.size()==6&&q.contains("key")&&q.contains("codec")&&q.contains("encoded_bytes")&&q.contains("encoded_sha256")&&q.contains("decoded_bytes")&&q.contains("decoded_sha256"),"exact frame declaration fields");
 need(q.at("key").is_string()&&!q.at("key").get_ref<const std::string&>().empty()&&q.at("key").get_ref<const std::string&>().size()<=64,"bounded key");need(q.at("codec")=="zlib","known lossless codec");U en=natural(q.at("encoded_bytes")),de=natural(q.at("decoded_bytes"));need(en>0&&en<=CAP,"encoded frame cap");need(de>0&&de<=CAP,"decoded frame cap");need(sha_field(q.at("encoded_sha256"))&&sha_field(q.at("decoded_sha256")),"lowercase SHA256 fields");
}
// Compressed bytes alone persist. The scoped callback owns any typed model it
// constructs; decoded bytes cannot be returned by reference or borrowed pointer.
class Cache {
 std::map<std::string,J>declared_;std::map<std::string,std::string>encoded_;bool decoding_=false;
 U encoded_bytes_=0,decoded_declared_=0,decoded_seen_=0,decode_calls_=0,peak_decoded_=0,live_decoded_=0;
 J decoded_receipts_=J::array();
 public:
 explicit Cache(const J&manifest){need(manifest.is_array()&&!manifest.empty()&&manifest.size()<=8,"one through eight frames");U sum=0;for(const auto&q:manifest){declaration(q);U n=natural(q.at("encoded_bytes"));need(n<=CAP-sum,"encoded aggregate cap");sum+=n;decoded_declared_+=natural(q.at("decoded_bytes"));need(declared_.emplace(q.at("key").get<std::string>(),q).second,"unique frame key");}}
 void read_one(std::istream&in){J q=frame_bridge::parse(frame_bridge::line(in,4096));declaration(q);std::string key=q.at("key");need(declared_.count(key)&&!encoded_.count(key),"known unread frame");need(q==declared_.at(key),"exact declared frame header");U n=natural(q.at("encoded_bytes"));need(n<=CAP-encoded_bytes_,"encoded aggregate cap");std::string bytes(n,'\0');for(U off=0;off<n;){U take=std::min<U>(65536,n-off);in.read(&bytes[off],std::streamsize(take));need(U(in.gcount())==take,"truncated encoded payload");off+=take;}need(tiny_sha::sha256(bytes)==q.at("encoded_sha256").get<std::string>(),"encoded SHA mismatch");encoded_.emplace(key,std::move(bytes));encoded_bytes_+=n;}
 void finish(std::istream&in){need(encoded_.size()==declared_.size(),"missing encoded frame");need(in.peek()==std::char_traits<char>::eof(),"trailing transport bytes");}
 bool contains(const std::string&key)const{return encoded_.count(key)!=0;}
 U size()const{return encoded_.size();}
 J decoded_manifest()const{J rows=J::array();for(const auto&i:declared_)rows.push_back({{"key",i.first},{"bytes",i.second.at("decoded_bytes")},{"sha256",i.second.at("decoded_sha256")}});return rows;}
 template<class Fn>auto with_decoded(const std::string&key,Fn fn)->std::invoke_result_t<Fn,const std::string&>{
  using R=std::invoke_result_t<Fn,const std::string&>;static_assert(!std::is_reference_v<R>&&!std::is_pointer_v<R>,"no borrowed decoded data may escape");
  need(!decoding_,"only one live decoded frame");need(encoded_.count(key),"cached key required");const auto&q=declared_.at(key);U n=natural(q.at("decoded_bytes"));decoding_=true;live_decoded_=n;peak_decoded_=std::max(peak_decoded_,n);
  struct Release {Cache&c;~Release(){c.live_decoded_=0;c.decoding_=false;}}release{*this};
  std::string raw(n,'\0');const auto&bytes=encoded_.at(key);z_stream z{};need(inflateInit(&z)==Z_OK,"zlib initialization");struct End {z_stream&z;~End(){inflateEnd(&z);}}end{z};z.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));z.avail_in=uInt(bytes.size());z.next_out=reinterpret_cast<Bytef*>(raw.data());z.avail_out=uInt(raw.size());
  int status=inflate(&z,Z_FINISH);need(status==Z_STREAM_END&&z.total_in==bytes.size()&&z.total_out==raw.size(),"exact single zlib stream and decoded length");need(tiny_sha::sha256(raw)==q.at("decoded_sha256").get<std::string>(),"decoded original JSON SHA mismatch");++decode_calls_;decoded_seen_+=n;decoded_receipts_.push_back({{"key",key},{"bytes",n},{"sha256",q.at("decoded_sha256")}});
  if constexpr(std::is_void_v<R>){fn(raw);return;}else{return fn(raw);}
 }
 J receipt()const{return {{"schema","RUNTIME_ZLIB_RAM_FRAME_RECEIPT_V1"},{"zlib_runtime_version",zlibVersion()},{"frames",encoded_.size()},{"encoded_payload_bytes",encoded_bytes_},{"decoded_declared_aggregate_bytes",decoded_declared_},{"decoded_consumed_bytes",decoded_seen_},{"decode_calls",decode_calls_},{"peak_single_decoded_bytes",peak_decoded_},{"live_decoded_bytes",live_decoded_},{"encoded_aggregate_cap",CAP},{"single_decoded_cap",CAP},{"decoded_aggregate_is_encoded_size",false},{"decoded_receipts",decoded_receipts_},{"payloads_saved",false}};}
};
inline J read_control(std::istream&in){J c=frame_bridge::parse(frame_bridge::line(in,262144));need(c.is_object()&&c.size()==3&&c.contains("schema")&&c.contains("frames")&&c.contains("decoded_control"),"exact transport control fields");need(c.at("schema")=="RUNTIME_ZLIB_RAM_FRAMES_V1","transport schema");return c;}
}
