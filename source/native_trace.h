#pragma once
// Ordered post-cache requests. NativeCosim and FunctionalDirect are distinct
// formats of execution, never interchangeable timing or cache-order evidence.
#include "work/tilegen-full-r1/driver-pooled-fusednorm-r1/sha256.h"
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace native_trace {
using U=std::uint64_t;
inline constexpr U unknown=UINT64_MAX, default_max_bytes=16ULL<<30;
inline constexpr U header_bytes=96, record_bytes=144, footer_bytes=128;
enum class Mode:U { NativeCosim=0, FunctionalDirect=1 };
enum class Cause:U { ReadFillOrRfo=0, DirtyWriteback=1 };
struct Record {
    U request_id=0,source_sequence=0;
    U issue_cycle=unknown,issue_ps=unknown,admission_cycle=unknown,admission_ps=unknown;
    U source_matrix_id=unknown,source_line_address=0,service_address=0,bytes=0;
    U node_id=unknown,sm_id=unknown,l2_subpartition_id=unknown;
    Cause cause=Cause::ReadFillOrRfo;
    U call_index=unknown,cta=unknown,warp=unknown,pc=unknown;
};
inline void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}
inline U add(U a,U b){need(b<=UINT64_MAX-a,"trace integer overflow");return a+b;}
inline bool hex_sha(const std::string& x){return x.size()==64&&x.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline const char* mode_name(Mode m){return m==Mode::NativeCosim?"NATIVE_COSIM_ADMITTED_REQUESTS":"FUNCTIONAL_DIRECT_DETERMINISTIC_ORDER";}
inline std::array<U,18> words(const Record& r){return {{r.request_id,r.source_sequence,r.issue_cycle,r.issue_ps,r.admission_cycle,r.admission_ps,r.source_matrix_id,r.source_line_address,r.service_address,r.bytes,r.node_id,r.sm_id,r.l2_subpartition_id,U(r.cause),r.call_index,r.cta,r.warp,r.pc}};}
inline Record from_words(const std::array<U,18>& w){
    need(w[13]<=1,"unknown trace cause");
    return {w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],w[8],w[9],w[10],w[11],w[12],Cause(w[13]),w[14],w[15],w[16],w[17]};
}
inline void put(char* p,U x){for(unsigned i=0;i<8;++i)p[i]=char(x>>(8*i));}
inline U get(const char* p){U x=0;for(unsigned i=0;i<8;++i)x|=U(static_cast<unsigned char>(p[i]))<<(8*i);return x;}
struct Receipt {
    Mode mode=Mode::NativeCosim;std::string path,context_sha256,record_sha256,file_sha256;
    U records=0,read_requests=0,write_requests=0,read_bytes=0,write_bytes=0,file_bytes=0;
    U request_payload_fnv1a64=14695981039346656037ULL;
    nlohmann::json to_json()const{return {
        {"schema","TILEGEN_NATIVE_TRACE_RECEIPT_V1"},{"status","PASS_CLOSED_TRACE_READBACK"},
        {"mode",mode_name(mode)},{"path",path},{"context_sha256",context_sha256},
        {"records",records},{"read_requests",read_requests},{"write_requests",write_requests},
        {"read_bytes",read_bytes},{"write_bytes",write_bytes},{"file_bytes",file_bytes},
        {"record_sha256",record_sha256},{"file_sha256",file_sha256},
        {"request_payload_fnv1a64",request_payload_fnv1a64},
        {"record_bytes",record_bytes},{"unknown_u64",unknown},
        {"source_address_scope","SOURCE_OR_CANONICAL_LINE_KEY_NOT_HARDWARE_PHYSICAL_ADDRESS"},
        {"service_address_scope","ADDRESS_SUBMITTED_TO_NATIVE_MEMORY_SERVICE_OR_DECLARED_DIRECT_NAMESPACE"},
        {"read_cause_scope","LOAD_FILL_AND_STORE_RFO_NOT_DISTINGUISHED_BY_NATIVE_REQUEST_TYPE"}};}
};
struct Ledger {
    Receipt value;U previous_issue=0,previous_admission=0,previous_issue_ps=0,previous_admission_ps=0,previous_call=0;
    void accept(const Record& r){
        need(r.request_id==value.records&&r.source_sequence==r.request_id,"trace request IDs must be contiguous from zero");
        need(r.call_index!=unknown&&(!value.records||r.call_index>=previous_call),"trace kernel context missing or regressed");
        need(r.source_line_address%128==0,"unaligned source line key");
        need(r.cause==Cause::ReadFillOrRfo||r.cause==Cause::DirtyWriteback,"invalid trace cause");
        const bool wr=r.cause==Cause::DirtyWriteback;
        need(r.bytes==(wr?32U:128U)&&r.service_address%r.bytes==0,"trace requires aligned read128 or individual dirty32");
        need(r.service_address<=UINT64_MAX-(r.bytes-1),"trace service extent overflow");
        if(value.mode==Mode::FunctionalDirect){
            need(r.issue_cycle==unknown&&r.issue_ps==unknown&&r.admission_cycle==unknown&&r.admission_ps==unknown,"direct trace must not invent timing");
        }else{
            need(r.issue_cycle!=unknown&&r.issue_ps!=unknown&&r.admission_cycle!=unknown&&r.admission_ps!=unknown,"native trace timing missing");
            need(r.admission_cycle>=r.issue_cycle&&r.admission_ps>=r.issue_ps,"admission precedes issue");
            need(!value.records||(r.issue_cycle>=previous_issue&&r.admission_cycle>=previous_admission&&r.issue_ps>=previous_issue_ps&&r.admission_ps>=previous_admission_ps),"trace issue/admission ordering regressed");
        }
        value.records=add(value.records,1);
        if(wr){value.write_requests=add(value.write_requests,1);value.write_bytes=add(value.write_bytes,r.bytes);}
        else{value.read_requests=add(value.read_requests,1);value.read_bytes=add(value.read_bytes,r.bytes);}
        for(U x:{r.request_id,r.service_address,r.bytes,U(wr)})for(unsigned b=0;b<8;++b){value.request_payload_fnv1a64^=(x>>(8*b))&255;value.request_payload_fnv1a64*=1099511628211ULL;}
        previous_issue=r.issue_cycle;previous_admission=r.admission_cycle;previous_issue_ps=r.issue_ps;previous_admission_ps=r.admission_ps;previous_call=r.call_index;
    }
};
struct File {
    int fd=-1;
    explicit File(int x):fd(x){need(fd>=0,"trace file open failed");}
    ~File(){if(fd>=0)::close(fd);}
    File(const File&)=delete;File& operator=(const File&)=delete;
};
inline void same_file(const struct stat& a,const struct stat& b){
    need(a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_size==b.st_size&&a.st_mtime==b.st_mtime,"trace changed during readback");
}
inline Receipt validate(const std::string& path,const std::string& expected_file_sha256,U max_bytes=default_max_bytes,
                        const std::function<void(const Record&)>& visit={}){
    need(hex_sha(expected_file_sha256),"trusted trace file SHA required");
    need(max_bytes>=header_bytes+footer_bytes&&max_bytes<=(64ULL<<30),"trace validation cap");
    File file(::open(path.c_str(),O_RDONLY|O_NOFOLLOW));struct stat before{},after{};
    need(::fstat(file.fd,&before)==0&&S_ISREG(before.st_mode)&&before.st_size>=0&&U(before.st_size)<=max_bytes,"trace must be bounded regular file");
    tiny_sha::Sha256 full,records;U consumed=0;
    std::array<char,65536> input{};std::size_t available=0,offset=0;
    auto read=[&](char* out,std::size_t count){
        need(count<=max_bytes-consumed,"trace read cap exceeded");std::size_t done=0;
        while(done<count){
            if(offset==available){
                ssize_t n;do{n=::read(file.fd,input.data(),input.size());}while(n<0&&errno==EINTR);
                need(n>0,"truncated trace");offset=0;available=std::size_t(n);
            }
            const auto n=std::min(count-done,available-offset);std::memcpy(out+done,input.data()+offset,n);offset+=n;done+=n;
        }
        full.add(out,count);consumed+=count;
    };
    std::array<char,header_bytes> header{};read(header.data(),header.size());
    need(std::memcmp(header.data(),"TGCSIM01",8)==0&&get(header.data()+8)==record_bytes&&get(header.data()+24)==0,"trace header/version/record width");
    const U mode=get(header.data()+16);need(mode<=1,"trace mode");Ledger ledger;ledger.value.mode=Mode(mode);ledger.value.path=path;
    ledger.value.context_sha256=std::string(header.data()+32,64);need(hex_sha(ledger.value.context_sha256),"trace context SHA");
    for(;;){
        std::array<char,record_bytes> raw{};read(raw.data(),8);
        if(get(raw.data())==unknown){
            std::array<char,footer_bytes> footer{};std::memcpy(footer.data(),raw.data(),8);read(footer.data()+8,footer.size()-8);
            need(std::memcmp(footer.data()+8,"TGCSEND1",8)==0,"trace footer magic");
            const std::array<U,6> summary{{ledger.value.records,ledger.value.read_requests,ledger.value.write_requests,ledger.value.read_bytes,ledger.value.write_bytes,ledger.value.request_payload_fnv1a64}};
            for(std::size_t i=0;i<summary.size();++i)need(get(footer.data()+16+8*i)==summary[i],"trace footer census/hash mismatch");
            ledger.value.record_sha256=records.hex();need(std::string(footer.data()+64,64)==ledger.value.record_sha256,"trace record SHA mismatch");
            break;
        }
        read(raw.data()+8,raw.size()-8);std::array<U,18> values{};
        for(std::size_t i=0;i<values.size();++i)values[i]=get(raw.data()+8*i);
        const auto r=from_words(values);ledger.accept(r);records.add(raw.data(),raw.size());if(visit)visit(r);
    }
    need(offset==available,"trace trailing buffered bytes");
    char extra;ssize_t tail;do{tail=::read(file.fd,&extra,1);}while(tail<0&&errno==EINTR);
    need(tail==0,"trace trailing bytes or read failure");need(::fstat(file.fd,&after)==0,"trace final stat");same_file(before,after);
    need(consumed==U(before.st_size),"trace file size changed");ledger.value.file_bytes=consumed;ledger.value.file_sha256=full.hex();
    need(ledger.value.file_sha256==expected_file_sha256,"trace whole-file SHA mismatch");return ledger.value;
}
class Writer {
    File file_;Ledger ledger_;U cap_;std::array<char,65536> buffer_{};std::size_t buffered_=0;
    std::string partial_path_;
    tiny_sha::Sha256 full_,records_;U bytes_=0;bool finished_=false,failed_=false;
    void flush(){
        std::size_t done=0;while(done<buffered_){const auto n=::write(file_.fd,buffer_.data()+done,buffered_-done);if(n<0&&errno==EINTR)continue;need(n>0,"trace write failed");done+=std::size_t(n);}buffered_=0;
    }
    void emit(const char* data,std::size_t count){
        need(count<=cap_-bytes_,"trace file cap exceeded");full_.add(data,count);bytes_+=count;
        while(count){const auto n=std::min(count,buffer_.size()-buffered_);std::memcpy(buffer_.data()+buffered_,data,n);buffered_+=n;data+=n;count-=n;if(buffered_==buffer_.size())flush();}
    }
    static int create(const std::string& path,Mode mode,const std::string& context,U cap){
        need(mode==Mode::NativeCosim||mode==Mode::FunctionalDirect,"unknown trace mode");need(hex_sha(context),"64 lowercase hex trace context SHA required");
        need(cap>=header_bytes+footer_bytes&&cap<=(64ULL<<30),"trace cap must be 224B..64GiB");
        struct stat st{};need(::lstat(path.c_str(),&st)<0&&errno==ENOENT,"trace destination already exists or is inaccessible");
        return ::open((path+".partial").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    }
public:
    Writer(const std::string& path,Mode mode,const std::string& context_sha256,U max_bytes=default_max_bytes)
        :file_(create(path,mode,context_sha256,max_bytes)),cap_(max_bytes),partial_path_(path+".partial"){
        ledger_.value.path=path;ledger_.value.mode=mode;ledger_.value.context_sha256=context_sha256;
        std::array<char,header_bytes> header{};std::memcpy(header.data(),"TGCSIM01",8);put(header.data()+8,record_bytes);put(header.data()+16,U(mode));
        std::memcpy(header.data()+32,context_sha256.data(),64);emit(header.data(),header.size());
    }
    Writer(const Writer&)=delete;Writer& operator=(const Writer&)=delete;
    U count()const{return ledger_.value.records;}
    Mode mode()const{return ledger_.value.mode;}
    void append(const Record& r){
        need(!finished_&&!failed_,"trace writer is closed or failed");
        try{
            need(record_bytes+footer_bytes<=cap_-bytes_,"trace cap leaves no room for record and footer");
            ledger_.accept(r);const auto w=words(r);std::array<char,record_bytes> raw{};
            for(std::size_t i=0;i<w.size();++i)put(raw.data()+8*i,w[i]);records_.add(raw.data(),raw.size());emit(raw.data(),raw.size());
        }catch(...){failed_=true;throw;}
    }
    Receipt finish(){
        need(!finished_&&!failed_,"trace finish requires healthy open writer");
        try{
            std::array<char,footer_bytes> footer{};put(footer.data(),unknown);std::memcpy(footer.data()+8,"TGCSEND1",8);
            const auto& r=ledger_.value;const std::array<U,6> summary{{r.records,r.read_requests,r.write_requests,r.read_bytes,r.write_bytes,r.request_payload_fnv1a64}};
            for(std::size_t i=0;i<summary.size();++i)put(footer.data()+16+8*i,summary[i]);
            const auto hash=records_.hex();std::memcpy(footer.data()+64,hash.data(),64);emit(footer.data(),footer.size());flush();
            need(::fsync(file_.fd)==0,"trace fsync failed");const int fd=file_.fd;file_.fd=-1;need(::close(fd)==0,"trace close failed");
            auto checked=validate(partial_path_,full_.hex(),cap_);need(checked.records==r.records&&checked.record_sha256==hash&&checked.context_sha256==r.context_sha256,"trace readback changed");
            // Same-directory hard-link publication is atomic and never replaces
            // an existing destination. Interrupted/failed writers keep .partial.
            need(::link(partial_path_.c_str(),r.path.c_str())==0,"trace publish failed; partial retained");
            (void)::unlink(partial_path_.c_str());checked.path=r.path;
            finished_=true;return checked;
        }catch(...){failed_=true;throw;}
    }
};
} // namespace native_trace
