#include "scene/Naming.h"

#include "scene/Node.h"

#include <algorithm>
#include <optional>
#include <cctype>

namespace lila::scene
{
namespace
{
bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool isLower(char c) { return c >= 'a' && c <= 'z'; }

char lowerChar(char c) { return char(std::tolower(static_cast<unsigned char>(c))); }
char upperChar(char c) { return char(std::toupper(static_cast<unsigned char>(c))); }

/** Sides and positions: left, right, centre, middle, front, back, top, and the corners. */
constexpr const char* kDefaultSuffixes = "L, R, C, M, F, B, T, LF, RF, LB, RB, FL, FR, BL, BR";
/** Unreal asset and collision prefixes. */
constexpr const char* kDefaultPrefixes = "SM, SKM, SK, UCX, UBX, USP, UCP, SOCKET, MI, M, T";

/** "L, R" or ["L", "R"]. */
std::vector<std::string> parseSuffixes(const nlohmann::json& j)
{
    std::vector<std::string> out;
    auto add = [&](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '_'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '_'))
            s.remove_suffix(1);
        if (!s.empty())
            out.emplace_back(s);
    };
    if (j.is_array())
        for (const nlohmann::json& s : j)
            if (s.is_string())
                add(s.get<std::string>());
    if (j.is_string())
    {
        const std::string s = j.get<std::string>();
        for (size_t at = 0; at <= s.size();)
        {
            const size_t end = std::min(s.find_first_of(", ", at), s.size());
            add(std::string_view(s).substr(at, end - at));
            at = end + 1;
        }
    }
    return out;
}
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

std::string applyCase(std::string_view name, CaseStyle style, const Underscores& underscores)
{
    if (style == CaseStyle::Any)
        return std::string(name);
    if (underscores.allow && (style == CaseStyle::Pascal || style == CaseStyle::Camel) && name.find('_') != std::string_view::npos)
    {
        std::vector<std::string_view> parts;
        for (size_t at = 0;;)
        {
            const size_t end = std::min(name.find('_', at), name.size());
            parts.push_back(name.substr(at, end - at));
            if (end == name.size())
                break;
            at = end + 1;
        }
        auto listed = [&](size_t i) -> std::optional<std::string> {
            for (const std::string& s : underscores.suffixes)
                if (s.size() == parts[i].size() && std::ranges::equal(s, parts[i], {}, lowerChar, lowerChar))
                    return s;
            return std::nullopt;
        };
        // The part as a listed suffix, or nothing. Numbers count, and so does the empty part before uniqueness digits.
        auto suffix = [&](size_t i) -> std::optional<std::string> {
            std::string_view p = parts[i];
            if (p.empty() ? i + 1 == parts.size() : std::ranges::all_of(p, isDigit))
                return std::string(p);
            return listed(i);
        };
        size_t body = 1;
        if (underscores.suffixesOnly)
        {
            body = parts.size();
            while (body > 1 && suffix(body - 1))
                --body;
        }
        std::string head;
        for (size_t i = 0; i < body; ++i)
            head += std::string(parts[i]) + " ";
        std::string out = applyCase(head, style);
        for (size_t i = body; i < parts.size(); ++i)
        {
            std::optional<std::string> s = suffix(i);
            std::string part = s ? *s : applyCase(parts[i], CaseStyle::Pascal);
            // A part that starts with a digit keeps it in front: arm_2b is Arm_2b, not Arm__2b.
            if (!s && !part.empty() && part[0] == '_' && !parts[i].empty() && isDigit(parts[i][0]))
                part.erase(0, 1);
            out += "_" + part;
        }
        return out;
    }
    std::vector<std::string> words = splitWords(name);
    std::string out;
    for (size_t i = 0; i < words.size(); ++i)
    {
        std::string w = words[i];
        for (char& c : w)
            c = style == CaseStyle::UpperSnake ? upperChar(c) : lowerChar(c);
        if (style == CaseStyle::Pascal || (style == CaseStyle::Camel && i > 0))
            w[0] = upperChar(w[0]);
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
        {"allowUnderscores", {{"joint", true}, {"*", true}}},
        {"underscoreSuffixesOnly", true},
        {"underscoreSuffixes", kDefaultSuffixes},
        {"rules", {{"joint", "snake_case"}, {"*", "PascalCase"}}},
        {"prefixes", kDefaultPrefixes},
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
    p.allowUnderscores.clear();
    bool fallbackUnderscores = true;
    for (auto& [type, allow] : j["allowUnderscores"].items())
    {
        if (!allow.is_boolean())
            continue;
        if (type == "*")
            fallbackUnderscores = allow.get<bool>();
        else
            p.allowUnderscores.emplace_back(type, allow.get<bool>());
    }
    p.allowUnderscores.emplace_back("*", fallbackUnderscores);
    p.underscoreSuffixesOnly = j.value("underscoreSuffixesOnly", true);
    p.underscoreSuffixes = parseSuffixes(j["underscoreSuffixes"]);
    std::string mode = j.value("mode", "fix");
    p.mode = mode == "warn" ? Mode::Warn : mode == "block" ? Mode::Block : Mode::Fix;
    p.rules.clear();
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
    p.prefixes = parseSuffixes(j["prefixes"]);
    return p;
}

Underscores NamingPolicy::underscoresFor(const NodeType& type) const
{
    Underscores u{.allow = true, .suffixesOnly = underscoreSuffixesOnly, .suffixes = underscoreSuffixes};
    for (auto& [t, allow] : allowUnderscores)
        if (t == "*" || type.isA(t))
        {
            u.allow = allow;
            break;
        }
    return u;
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
    while (true)
    {
        const std::string* match = nullptr;
        for (const std::string& pre : prefixes)
            if (shortName.size() > pre.size() + 1 && shortName[pre.size()] == '_' && (!match || pre.size() > match->size()) &&
                std::ranges::equal(pre, shortName.substr(0, pre.size()), {}, lowerChar, lowerChar))
                match = &pre;
        if (!match)
            break;
        prefix += *match + "_";
        shortName.remove_prefix(match->size() + 1);
    }
    CaseStyle style = styleFor(type);
    // SM_MyRock, not SM_myRock.
    if (!prefix.empty() && style == CaseStyle::Camel)
        style = CaseStyle::Pascal;

    // Uniqueness digits belong to the name as typed, not to a word.
    size_t digits = shortName.size();
    while (digits > 0 && isDigit(shortName[digits - 1]))
        --digits;
    std::string body = applyCase(shortName.substr(0, digits), style, underscoresFor(type));
    return ns + prefix + body + std::string(shortName.substr(digits));
}
} // namespace lila::scene
