#pragma once
#include <limits>
#include <tuple>

#ifndef TILEGEN_HOST_PIPELINE_FRONT_CACHE
#define TILEGEN_HOST_PIPELINE_FRONT_CACHE 1
#endif
#ifndef TILEGEN_HOST_FORECAST_READY_FLOOR
#define TILEGEN_HOST_FORECAST_READY_FLOOR 1
#endif

namespace GTSim::host_pipeline {
// The minimum of pipeline FIFO heads, not of all completion entries. A pop
// invalidates; a first enqueue into an empty FIFO can only lower the minimum.
template<class Cycle> struct FrontMinimum {
    Cycle earliest=std::numeric_limits<Cycle>::max();
    bool dirty=false;
    void invalidate(){dirty=true;}
    void first_entry(Cycle cycle){if(cycle<earliest)earliest=cycle;}
    template<class Pipelines> void rebuild(const Pipelines& pipelines){
        earliest=std::numeric_limits<Cycle>::max();
        for(const auto* pipe:pipelines)if(!pipe->pipeline_executing_nodes.empty())
            first_entry(std::get<2>(pipe->pipeline_executing_nodes.front()));
        dirty=false;
    }
};
}
