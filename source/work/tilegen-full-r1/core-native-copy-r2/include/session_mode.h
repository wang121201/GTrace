#pragma once
namespace tiny_runtime {
// Host execution strategy only; the scheduler and all cycle values are unchanged.
inline thread_local bool sp_event_mode = true;
}
