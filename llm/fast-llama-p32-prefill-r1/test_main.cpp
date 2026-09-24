#include "llama_p32_prefill.h"
#include <iostream>
#include <sstream>
int main(){using namespace source_cache;try{std::ostringstream snapshots;Runner r(snapshots);std::string line;while(std::getline(std::cin,line)){auto c=J::parse(line);if(c.at("type")=="llama_p32_prefill_program_v1")llama_p32_prefill::execute(r,c);else r.command(c);}J records=J::array();std::istringstream s(snapshots.str());while(std::getline(s,line))records.push_back(J::parse(line));std::cout<<J({{"summary",r.summary()},{"snapshots",records}}).dump()<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
