#pragma once

#include <cstdint>
#include <string>

namespace native_typed {

// Supplied host-capture identity only. Neither this value nor a well-formed
// digest authenticates a receipt/journal, an object allocation, or a native
// transfer. The capture factory must verify those evidence files separately.
struct CaptureIdentity {
    std::uint64_t pid=0,start_ticks=0,native_launch_id=0;
    std::string source_launch_key,phase,argument_payload_sha256,capture_receipt_sha256;
};

inline bool hash_text(const std::string& text) {
    return text.size()==64&&text.find_first_not_of("0123456789abcdef")==std::string::npos;
}

} // namespace native_typed
