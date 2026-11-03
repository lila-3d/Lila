#pragma once

#include <filesystem>
#include <string>

namespace linktest
{
/** An empty lila_link_<name> folder under the system temp folder. */
inline std::filesystem::path tempDir(const char* name)
{
    std::filesystem::path d = std::filesystem::temp_directory_path() / ("lila_link_" + std::string(name));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}
} // namespace linktest
