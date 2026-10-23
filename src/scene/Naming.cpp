#include "scene/Naming.h"

#include "scene/Node.h"

#include <cctype>

namespace lila::scene
{
namespace
{
bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool isLower(char c) { return c >= 'a' && c <= 'z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return char(std::tolower(static_cast<unsigned char>(c))); }
char upper(char c) { return char(std::toupper(static_cast<unsigned char>(c))); }
} // namespace

std::vector<std::string> splitWords(std::string_view name)
{
    std::vector<std::string> words;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty())
            words.push_back(std::move(cur));
        cur.clear();
    };
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (c == '_' || c == '-' || c == ' ' || c == '.')
        {
            flush();
            continue;
        }
        if (!cur.empty())
        {
            char prev = cur.back();
            bool next = i + 1 < name.size() && isLower(name[i + 1]);
            // myCube -> my|Cube, HTTPServer -> HTTP|Server, digits stay with the word before them.
            if (isUpper(c) && (isLower(prev) || isDigit(prev) || (isUpper(prev) && next)))
                flush();
        }
        cur += c;
    }
    flush();
    return words;
}

std::string applyCase(std::string_view name, CaseStyle style)
{
    if (style == CaseStyle::Any)
        return std::string(name);
    std::vector<std::string> words = splitWords(name);
    std::string out;
    for (size_t i = 0; i < words.size(); ++i)
    {
        std::string w = words[i];
        for (char& c : w)
            c = style == CaseStyle::UpperSnake ? upper(c) : lower(c);
        if (style == CaseStyle::Pascal || (style == CaseStyle::Camel && i > 0))
            w[0] = upper(w[0]);
        if ((style == CaseStyle::Snake || style == CaseStyle::UpperSnake) && i > 0 && !isDigit(w[0]))
            out += '_';
        out += w;
    }
    // A name can't start with a digit.
    if (!out.empty() && isDigit(out[0]))
        out.insert(out.begin(), '_');
    return out;
}

CaseStyle parseCaseStyle(std::string_view s)
{
    if (s == "PascalCase" || s == "pascal")
        return CaseStyle::Pascal;
    if (s == "camelCase" || s == "camel")
        return CaseStyle::Camel;
    if (s == "snake_case" || s == "snake")
        return CaseStyle::Snake;
    if (s == "UPPER_SNAKE" || s == "upper_snake")
        return CaseStyle::UpperSnake;
    return CaseStyle::Any;
}

nlohmann::json NamingPolicy::defaults()
{
    return {
        {"enabled", false},
        {"mode", "fix"},
        {"uniqueNames", false},
        {"rules", {{"joint", "snake_case"}, {"*", "PascalCase"}}},
        {"keepPrefixes", {"UCX_", "UBX_", "USP_", "UCP_", "SOCKET_", "SM_", "SK_", "MI_", "M_", "T_"}},
    };
}

NamingPolicy NamingPolicy::fromJson(const nlohmann::json& in)
{
    nlohmann::json j = defaults();
    if (in.is_object())
        j.merge_patch(in);

    NamingPolicy p;
    p.enabled = j.value("enabled", false);
    p.uniqueNames = j.value("uniqueNames", false);
    std::string mode = j.value("mode", "fix");
    p.mode = mode == "warn" ? Mode::Warn : mode == "block" ? Mode::Block : Mode::Fix;
    std::pair<std::string, CaseStyle> fallback{"*", CaseStyle::Any};
    for (auto& [type, style] : j["rules"].items())
    {
        if (!style.is_string())
            continue;
        if (type == "*")
            fallback = {type, parseCaseStyle(style.get<std::string>())};
        else
            p.rules.emplace_back(type, parseCaseStyle(style.get<std::string>()));
    }
    p.rules.push_back(fallback);
    for (const auto& pre : j["keepPrefixes"])
        if (pre.is_string())
            p.keepPrefixes.push_back(pre.get<std::string>());
    return p;
}

CaseStyle NamingPolicy::styleFor(const NodeType& type) const
{
    for (auto& [t, style] : rules)
        if (t == "*" || type.isA(t))
            return style;
    return CaseStyle::Any;
}

std::string NamingPolicy::conform(const NodeType& type, std::string_view name) const
{
    if (!enabled)
        return std::string(name);

    std::string ns;
    std::string_view shortName = name;
    if (auto colon = name.rfind(':'); colon != std::string_view::npos)
    {
        ns = std::string(name.substr(0, colon + 1));
        shortName = name.substr(colon + 1);
    }

    std::string prefix;
    for (const std::string& pre : keepPrefixes)
        if (shortName.starts_with(pre) && pre.size() > prefix.size())
            prefix = pre;
    shortName.remove_prefix(prefix.size());

    // Uniqueness digits belong to the name as typed, not to a word.
    size_t digits = shortName.size();
    while (digits > 0 && isDigit(shortName[digits - 1]))
        --digits;
    std::string body = applyCase(shortName.substr(0, digits), styleFor(type));
    return ns + prefix + body + std::string(shortName.substr(digits));
}
} // namespace lila::scene
