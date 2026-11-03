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
        size_t first = 0, body = 1;
        if (underscores.suffixesOnly)
        {
            body = parts.size();
            while (body > 1 && suffix(body - 1))
                --body;
            if (underscores.prefixes)
                while (first + 1 < body && listed(first))
                    ++first;
        }
        std::string out;
        for (size_t i = 0; i < first; ++i)
            out += *listed(i) + "_";
        std::string head;
        for (size_t i = first; i < body; ++i)
            head += std::string(parts[i]) + " ";
        out += applyCase(head, first ? CaseStyle::Pascal : style);
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
        {"underscorePrefixes", false},
        {"underscoreSuffixes", kDefaultSuffixes},
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
    p.underscorePrefixes = j.value("underscorePrefixes", false);
    p.underscoreSuffixes = parseSuffixes(j["underscoreSuffixes"]);
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

Underscores NamingPolicy::underscoresFor(const NodeType& type) const
{
    Underscores u{.allow = true, .suffixesOnly = underscoreSuffixesOnly, .prefixes = underscorePrefixes, .suffixes = underscoreSuffixes};
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
    for (const std::string& pre : keepPrefixes)
        if (shortName.starts_with(pre) && pre.size() > prefix.size())
            prefix = pre;
    shortName.remove_prefix(prefix.size());

    // Uniqueness digits belong to the name as typed, not to a word.
    size_t digits = shortName.size();
    while (digits > 0 && isDigit(shortName[digits - 1]))
        --digits;
    std::string body = applyCase(shortName.substr(0, digits), styleFor(type), underscoresFor(type));
    return ns + prefix + body + std::string(shortName.substr(digits));
}
} // namespace lila::scene
