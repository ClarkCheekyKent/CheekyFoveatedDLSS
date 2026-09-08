#pragma once
#include "settings.hpp"
#include <filesystem>
#include <string>
#include <string_view>

namespace cheeky::foveated_dlss {
// Same keys as the ReShade add-on, independent of either host's UI API.
bool set_named_setting(Settings&, std::string_view key, std::string_view value);
std::string serialize_settings(const Settings&);
bool read_settings_file(const std::filesystem::path&, Settings&, std::string& error);
bool write_settings_file(const std::filesystem::path&, const Settings&, std::string& error);
std::string json_escape(std::string_view);
std::string settings_json(const Settings&);
}
