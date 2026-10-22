#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** The Qt-free half of torn-off menus: dock names, which identify a menu by its path in the menu bar. */
namespace lila::ui::tearoff
{
inline constexpr std::string_view kDockPrefix = "tearoff:";

/** "&Mesh" -> "Mesh", "Save && Close" -> "Save & Close". */
inline std::string stripMnemonic(std::string_view text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '&' && i + 1 < text.size())
            ++i;
        else if (text[i] == '&')
            continue;
        out += text[i];
    }
    return out;
}

/** A menu row's label: no mnemonic, no shortcut column (after a tab), no surrounding whitespace. */
inline std::string itemLabel(std::string_view text)
{
    std::string s = stripMnemonic(text.substr(0, text.find('\t')));
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

/** "tearoff:Mesh/Symmetry" for the labels {"Mesh", "Symmetry"} (see itemLabel). '/' and '%' in a label are percent-escaped. */
inline std::string dockName(const std::vector<std::string>& path)
{
    std::string out(kDockPrefix);
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (i)
            out += '/';
        for (char c : path[i])
            out += c == '/' ? std::string("%2F") : c == '%' ? std::string("%25") : std::string(1, c);
    }
    return out;
}

/** The menu path a dock name stands for, or nullopt when it is not a torn-off menu's. */
inline std::optional<std::vector<std::string>> parseDockName(std::string_view name)
{
    if (!name.starts_with(kDockPrefix) || name.size() == kDockPrefix.size())
        return std::nullopt;
    std::vector<std::string> path(1);
    for (size_t i = kDockPrefix.size(); i < name.size(); ++i)
    {
        char c = name[i];
        if (c == '/')
            path.emplace_back();
        else if (c == '%' && name.substr(i, 3) == "%2F")
        {
            path.back() += '/';
            i += 2;
        }
        else if (c == '%' && name.substr(i, 3) == "%25")
        {
            path.back() += '%';
            i += 2;
        }
        else
            path.back() += c;
    }
    for (const std::string& p : path)
        if (p.empty())
            return std::nullopt;
    return path;
}

/** Torn-off docks a saved layout's "docks" shows open, to recreate before its window state is restored. */
inline std::vector<std::string> openDocks(const nlohmann::json& docks)
{
    std::vector<std::string> out;
    if (!docks.is_object())
        return out;
    for (auto& [name, entry] : docks.items())
        if (parseDockName(name) && entry.is_object() && entry.value("open", false))
            out.push_back(name);
    return out;
}
} // namespace lila::ui::tearoff
