#pragma once
namespace cheeky::foveated_dlss {
// Varjo's SteamVR driver does not forward eye tracking to SteamVR. While runtime
// gaze is selected, join the running Varjo runtime directly for gaze samples.
// Loads the installed VarjoLib.dll on demand; never ships or initializes Varjo Base.
void poll_varjo_gaze(bool wanted) noexcept;
// Latest combined gaze direction in OpenVR head space (-Z forward). available
// reports whether a Varjo gaze session exists, independent of sample validity.
bool read_varjo_gaze(float ray[3], bool& available) noexcept;
void stop_varjo_gaze() noexcept;
}
