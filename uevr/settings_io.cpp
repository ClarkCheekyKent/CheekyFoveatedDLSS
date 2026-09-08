#include "settings_io.hpp"
#include <Windows.h>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <type_traits>

namespace cheeky::foveated_dlss {
namespace {
template<class T> bool parse(std::string_view s, T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        if (s == "true" || s == "1") { value = true; return true; }
        if (s == "false" || s == "0") { value = false; return true; }
        return false;
    } else if constexpr (std::is_enum_v<T>) {
        std::uint32_t n{};
        if (!parse(s, n) || n > 2) return false;
        value = static_cast<T>(n); return true;
    } else {
        T n{};
        const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
        if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(n)) return false;
        value = n; return true;
    }
}
template<class T> auto number(T n) {
    if constexpr (std::is_enum_v<T>) return static_cast<std::uint32_t>(n);
    else return n;
}
std::string_view trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == s.npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
}
bool set_named_setting(Settings& s, std::string_view key, std::string_view value) {
#define CHEEKY_SETTING(name, field) if (key == name) return parse(value, s.field);
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    return false;
}
std::string serialize_settings(const Settings& s) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(9);
    out << "[CheekyFoveatedDLSS]\nSchemaVersion=1\n";
#define CHEEKY_SETTING(name, field) out << name << '=' << number(s.field) << '\n';
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    return out.str();
}
std::string settings_json(const Settings& s) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(9) << std::boolalpha;
    out << '{'; bool first = true;
#define CHEEKY_SETTING(name, field) if (!first) out << ','; first = false; out << '"' << name << "\":" << number(s.field);
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    out << '}'; return out.str();
}
std::string json_escape(std::string_view s) {
    std::string out; constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out;
}
bool read_settings_file(const std::filesystem::path& path, Settings& s, std::string& error) {
    std::ifstream in(path);
    if (!in) { error = "Cannot read settings file"; return false; }
    auto candidate = s; std::string line; bool section = false;
    while (std::getline(in, line)) {
        const auto text = trim(line);
        if (text.empty() || text.front() == ';' || text.front() == '#') continue;
        if (text.front() == '[') { section = text == "[CheekyFoveatedDLSS]"; continue; }
        if (!section) continue;
        const auto split = text.find('='); if (split == text.npos) continue;
        const auto key = trim(text.substr(0, split)), value = trim(text.substr(split + 1));
        if (key == "SchemaVersion") {
            if (value != "1") { error = "Unsupported settings schema"; return false; }
        } else {
            // Unknown future keys are ignored, but malformed known values reject
            // the file as a transaction instead of applying half a configuration.
            bool known = false;
#define CHEEKY_SETTING(name, field) known |= key == name;
#include "settings_fields.inc"
#undef CHEEKY_SETTING
            if (known && !set_named_setting(candidate, key, value)) {
                error = "Invalid setting: " + std::string(key); return false;
            }
        }
    }
    if (in.bad()) { error = "Settings read failed"; return false; }
    s = candidate; error.clear(); return true;
}
bool write_settings_file(const std::filesystem::path& path, const Settings& s, std::string& error) {
    auto temporary = path; temporary += L".tmp";
    { std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      out << serialize_settings(s); out.flush();
      if (!out) { error = "Cannot write settings file"; return false; } }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace settings file"; return false;
    }
    error.clear(); return true;
}
}
