#pragma once
#include "cheeky_gaze_abi.h"
#include "settings.hpp"
#include <Unknwn.h>
namespace cheeky::foveated_dlss {
// Called by the existing interception worker, after MinHook initialization.
void poll_openvr_hooks() noexcept;
// Native hosts can join an already initialized OpenVR session whose compositor
// the game cached before Cheeky loaded. Never initializes a runtime; disabled
// for legacy hosts and suppressed when ReShade owns interface selection.
void enable_openvr_late_recovery(bool enabled) noexcept;
// The host has already selected this compositor. Match its exact object to a
// supported ABI before touching its vtable; never guess the host's SDK version.
bool attach_openvr_compositor(void* compositor) noexcept;
// Called after worker exit and before global MinHook teardown.
void stop_openvr_hooks() noexcept;
bool read_openvr_gaze(const Settings&, IUnknown*, CheekyGazeSnapshotV1&,std::uint64_t native_identity=0) noexcept;
}
