#pragma once

#include "core/Export.h"
#include "ops/Command.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Named option box settings a user saves for a command, shared as one small file each:
 *   {"format": "lilaOptions", "version": 1, "command": "quadRemesh", "name": "Crowd Character", "values": {flag: value}}
 * Files live in optionPresets/<command>/ under each studio folder (read only here), the user's config dir and the open
 * workspace's .lila/ (versioned with the project, so a team shares it). Same-named presets: the later folder wins.
 */
namespace lila::ops::options::presets
{
struct Preset
{
    std::string name;
    /** "studio", "user" or "project". */
    std::string source;
    std::filesystem::path file;
    json values = json::object();
    /** Only the user's and the project's are saved over or deleted from Lila. */
    bool writable = false;
};

/** The folder source keeps command's presets in; empty for "project" without an open workspace. */
LILA_FOUNDATION_API std::filesystem::path dir(std::string_view command, std::string_view source);
/** Every preset for command, studio first, then user, then project; a name repeated by a later folder replaces the earlier. */
LILA_FOUNDATION_API std::vector<Preset> list(std::string_view command);
LILA_FOUNDATION_API std::optional<Preset> find(std::string_view command, std::string_view name);
/** Writes values as name into source's folder ("user" or "project"), replacing a preset of that name there. */
LILA_FOUNDATION_API std::optional<Preset> save(std::string_view command, std::string_view name, const json& values,
                                               std::string_view source = "user", std::string* error = nullptr);
LILA_FOUNDATION_API bool remove(const Preset& p, std::string* error = nullptr);
/** Copies a preset file into the user's folder for the command it names. */
LILA_FOUNDATION_API std::optional<Preset> install(const std::filesystem::path& file, std::string* error = nullptr);
/** Reads one preset file. */
LILA_FOUNDATION_API std::optional<Preset> load(const std::filesystem::path& file, std::string* error = nullptr);
} // namespace lila::ops::options::presets
