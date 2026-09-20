#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cheeky::foveated_dlss {
struct SupportFile { std::string name, contents; };

// Small, bounded ZIP32 archive using the standard STORE method (no dependency).
// Callers supply ASCII flat filenames and at most 20 MiB of report data.
inline void write_support_zip(const std::filesystem::path& path,
                              const std::vector<SupportFile>& files) {
    if (files.size() > 256U)
        throw std::runtime_error("Support archive exceeds 256 files");
    std::size_t total{};
    for (const auto& file : files) {
        if (file.contents.size() > 20U * 1024U * 1024U - total)
            throw std::runtime_error("Support archive exceeds 20 MiB");
        total += file.contents.size();
        if (file.name.empty() || file.name.size() > 255U ||
            file.name.find_first_of("/\\:") != std::string::npos)
            throw std::runtime_error("Invalid filename in support archive: " + file.name);
    }
    // Validate before opening/truncating the destination, including .partial reports.
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    auto u16 = [&](std::uint16_t n) {
        out.put(static_cast<char>(n)); out.put(static_cast<char>(n >> 8));
    };
    auto u32 = [&](std::uint32_t n) {
        u16(static_cast<std::uint16_t>(n)); u16(static_cast<std::uint16_t>(n >> 16));
    };
    struct Entry { std::uint32_t crc, size, offset; std::string name; };
    std::vector<Entry> entries;
    for (const auto& file : files) {
        std::uint32_t crc = 0xffffffffU;
        for (unsigned char c : file.contents) {
            crc ^= c;
            for (int bit = 0; bit != 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
        Entry e{~crc, static_cast<std::uint32_t>(file.contents.size()),
                static_cast<std::uint32_t>(out.tellp()), file.name};
        u32(0x04034b50); u16(20); u16(0); u16(0); u16(0); u16(0x21);
        u32(e.crc); u32(e.size); u32(e.size);
        u16(static_cast<std::uint16_t>(e.name.size())); u16(0);
        out.write(e.name.data(), e.name.size());
        out.write(file.contents.data(), file.contents.size());
        entries.push_back(e);
    }
    const auto directory = static_cast<std::uint32_t>(out.tellp());
    for (const auto& e : entries) {
        u32(0x02014b50); u16(20); u16(20); u16(0); u16(0); u16(0); u16(0x21);
        u32(e.crc); u32(e.size); u32(e.size);
        u16(static_cast<std::uint16_t>(e.name.size()));
        u16(0); u16(0); u16(0); u16(0); u32(0); u32(e.offset);
        out.write(e.name.data(), e.name.size());
    }
    const auto size = static_cast<std::uint32_t>(out.tellp()) - directory;
    u32(0x06054b50); u16(0); u16(0);
    u16(static_cast<std::uint16_t>(entries.size()));
    u16(static_cast<std::uint16_t>(entries.size()));
    u32(size); u32(directory); u16(0);
    out.close();
}
}
