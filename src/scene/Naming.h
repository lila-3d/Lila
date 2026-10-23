#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
struct NodeType;

enum class CaseStyle
{
    Any,
    Pascal,    // MyCube
    Camel,     // myCube
    Snake,     // my_cube
    UpperSnake // MY_CUBE
};

/** Splits any of myCube, MyCube, my_cube, my-cube, "my cube", HTTPServer2 into words. */
LILA_SCENE_API std::vector<std::string> splitWords(std::string_view name);
LILA_SCENE_API std::string applyCase(std::string_view name, CaseStyle style);
LILA_SCENE_API CaseStyle parseCaseStyle(std::string_view s);

/**
 * Per-workspace naming rules (prefs file "naming", usually in .lila/naming.json):
 *   enabled: bool
 *   mode: "fix" (rewrite to match) | "warn" | "block"
 *   rules: { "<nodeType>": "snake_case" | "PascalCase" | ..., "*": default }
 *   keepPrefixes: ["UCX_", "SOCKET_", ...]   kept verbatim in front of the converted name
 *   uniqueNames: bool   names unique across the whole scene; off, only among siblings (paths tell them apart)
 * Namespaces and trailing digits are left alone.
 */
struct LILA_SCENE_API NamingPolicy
{
    bool enabled = false;
    enum class Mode
    {
        Fix,
        Warn,
        Block
    } mode = Mode::Fix;
    std::vector<std::pair<std::string, CaseStyle>> rules = {{"joint", CaseStyle::Snake}, {"*", CaseStyle::Pascal}}; // "*" last
    std::vector<std::string> keepPrefixes;
    bool uniqueNames = false;

    static NamingPolicy fromJson(const nlohmann::json& j);
    static nlohmann::json defaults();

    CaseStyle styleFor(const NodeType& type) const;
    /** What the name should be under this policy (may equal name). */
    std::string conform(const NodeType& type, std::string_view name) const;
};
} // namespace lila::scene
