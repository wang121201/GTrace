// Closed actual role capacities; generated from pinned aggregate template receipts.
struct Profile { const char* name; int grid,perwarp,pcmax,shared,regs; std::array<U,3> sizes,strides; std::array<int,4> memcounts; U read,write,copies,zero,tensors,ranges,edges; };
inline Profile profile(const std::string& s){
 if(s=="o")return {"o",32,9901,14272,98304,158,{33554432,262144,262144},{1048576,0,256},{662,662,646,646},1310720,8192,2584,24,16384,374784,246356};
 if(s=="gate")return {"gate",224,10913,12272,73728,142,{234881024,262144,1835008},{1048576,0,256},{661,661,645,645},1310720,8192,2580,20,16384,373888,239696};
 if(s=="down")return {"down",32,32571,14272,73728,158,{117440512,917504,262144},{3670016,0,256},{2260,2260,2244,2244},4587520,8192,8976,16,57344,1275648,828152};
 throw std::runtime_error("closed three-role profile");
}
