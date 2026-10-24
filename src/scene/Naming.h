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
/** Which underscores PascalCase and camelCase keep. */
struct Underscores
{
    bool allow = false;
    /** Only the ones before trailing suffixes (or numbers): Arm_L and Wheel_LF stay, Dog_Leg_B becomes DogLeg_B. */
    bool suffixesOnly = true;
    /** With suffixesOnly, listed suffixes may also lead the name: L_Arm. */
    bool prefixes = false;
    std::vector<std::string> suffixes; // matched ignoring case and written as listed
};

/** With underscores allowed, PascalCase and camelCase case each kept underscore-separated part on its own (arm_l is Arm_L). */
LILA_SCENE_API std::string applyCase(std::string_view name, CaseStyle style, const Underscores& underscores = {});
LILA_SCENE_API CaseStyle parseCaseStyle(std::string_view s);

/**
 * Per-workspace naming rules (prefs file "naming", usually in .lila/naming.json):
 *   enabled: bool
 *   mode: "fix" (rewrite to match) | "warn" | "block"
 *   rules: { "<nodeType>": "snake_case" | "PascalCase" | ..., "*": default }
 *   keepPrefixes: ["UCX_", "SOCKET_", ...]   kept verbatim in front of the converted name
 *   uniqueNames: bool   names unique across the whole scene; off, only among siblings (paths tell them apart)
 *   allowUnderscores: { "<nodeType>": bool, "*": default }   PascalCase and camelCase keep typed underscores (Arm_L, not ArmL)
 *   underscoreSuffixesOnly: bool   kept underscores must come before a suffix or a number (Arm_L, not Dog_Leg_B)
 *   underscoreSuffixes: "L, R, ..." or ["L", "R", ...]   the suffixes that may follow an underscore
 *   underscorePrefixes: bool   the same suffixes may also lead a name, before an underscore (L_Arm)
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
    std::vector<std::pair<std::string, bool>> allowUnderscores = {{"joint", true}, {"*", true}}; // "*" last
    bool underscoreSuffixesOnly = true;
    bool underscorePrefixes = false;
    std::vector<std::string> underscoreSuffixes;

    static NamingPolicy fromJson(const nlohmann::json& j);
    static nlohmann::json defaults();

    CaseStyle styleFor(const NodeType& type) const;
    Underscores underscoresFor(const NodeType& type) const;
    /** What the name should be under this policy (may equal name). */
    std::string conform(const NodeType& type, std::string_view name) const;
};
} // namespace lila::scene
