#pragma once
#include "cheeky_fg_bridge.hpp"
// Optional quad-specific producer ABI. D3D11 resources are borrowed only for
// this call; the layer snapshots them before the game reuses the guides.
struct CheekyQuadFgSourceV1 {
    unsigned abi{1},size{sizeof(CheekyQuadFgSourceV1)};
    unsigned long long generation{};
    void* context{};void* depth{};void* motion{};
    CheekyFgEye meta;
    unsigned queue_priority{2},release_point{3},submit_repeats{}/*retired; ABI layout*/,frame_id{};
};
using CheekyQuadFgObserveFn=bool(__cdecl*)(const CheekyQuadFgSourceV1*);
struct CheekyQuadFgStatsV1 {
    unsigned abi{1},size{sizeof(CheekyQuadFgStatsV1)},enabled{},capture_stage{},assembly_stage{};
    unsigned long long observations{},snapshots{},frames{},guide_pairs{};
};
using CheekyQuadFgStatsFn=bool(__cdecl*)(CheekyQuadFgStatsV1*);
