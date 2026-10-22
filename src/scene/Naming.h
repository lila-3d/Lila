#pragma once

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
std::vector<std::string> splitWords(std::string_view name);
std::string applyCase(std::string_view name, CaseStyle style);
CaseStyle parseCaseStyle(std::string_view s);

/**
 * Per-workspace naming rules (prefs file "naming", usually in .lila/naming.json):
 *   enabled: bool
 *   mode: "fix" (rewrite to match) | "warn" | "block"
 *   rules: { "<nodeType>": "snake_case" | "PascalCase" | ..., "*": default }
 *   keepPrefixes: ["UCX_", "SOCKET_", ...]   kept verbatim in front of the converted name
 * Namespaces and trailing digits are left alone.
 */
struct NamingPolicy
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

    static NamingPolicy fromJson(const nlohmann::json& j);
    static nlohmann::json defaults();

    CaseStyle styleFor(const NodeType& type) const;
    /** What the name should be under this policy (may equal name). */
    std::string conform(const NodeType& type, std::string_view name) const;
};
} // namespace lila::scene
