#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
// Same integer JSON array bytes as nlohmann::json::array(...).dump()+"\n".
// No digest, order, value or sampling change; only serialization allocation.
namespace semantic_audit {
template<std::size_t N> inline void integer_line(std::string& out,const std::array<std::int64_t,N>& fields) {
    out.clear();out.push_back('[');
    for(std::size_t i=0;i<N;++i) {
        if(i)out.push_back(',');char digits[32];const auto r=std::to_chars(digits,digits+sizeof digits,fields[i]);
        if(r.ec!=std::errc{})throw std::logic_error("integer audit serialization overflow");
        out.append(digits,r.ptr);
    }
    out.append("]\n");
}
}
