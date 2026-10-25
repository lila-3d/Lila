#include "ui/OutlinerFilter.h"

#include "core/Profile.h"
#include "scene/Node.h"
#include "ui/TokenCompleter.h"

#include <algorithm>
#include <cctype>

namespace lila::ui::outliner
{
namespace
{
char lower(char c)
{
    return char(std::tolower(static_cast<unsigned char>(c)));
}

std::string lowered(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}

bool isGlob(std::string_view s)
{
    return s.find_first_of("*?[") != std::string_view::npos;
}

/** Matches the class starting at pattern[i] == '['; sets end to the index after ']'. False for an unclosed class. */
bool classMatch(std::string_view pattern, size_t i, char c, bool& hit, size_t& end)
{
    size_t j = i + 1;
    bool negate = j < pattern.size() && (pattern[j] == '!' || pattern[j] == '^');
    if (negate)
        ++j;
    size_t first = j;
    bool in = false;
    // A ']' straight after the opening is a member, not the close.
    for (; j < pattern.size() && (pattern[j] != ']' || j == first); ++j)
    {
        char lo = lower(pattern[j]);
        char hi = lo;
        if (j + 2 < pattern.size() && pattern[j + 1] == '-' && pattern[j + 2] != ']')
        {
            hi = lower(pattern[j + 2]);
            j += 2;
        }
        if (lower(c) >= lo && lower(c) <= hi)
            in = true;
    }
    if (j >= pattern.size())
        return false;
    end = j + 1;
    hit = in != negate;
    return true;
}

std::vector<std::string> split(std::string_view text, bool commas)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : text)
    {
        if (std::isspace(static_cast<unsigned char>(c)) || (commas && c == ','))
        {
            if (!cur.empty())
                out.push_back(std::move(cur));
            cur.clear();
        }
        else
            cur += c;
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}
} // namespace

bool globMatch(std::string_view pattern, std::string_view text)
{
    size_t p = 0, t = 0;
    size_t starP = std::string_view::npos, starT = 0;
    while (t < text.size())
    {
        if (p < pattern.size() && pattern[p] == '*')
        {
            starP = p++;
            starT = t;
            continue;
        }
        if (p < pattern.size())
        {
            bool hit = false;
            size_t next = p + 1;
            if (pattern[p] != '[' || !classMatch(pattern, p, text[t], hit, next))
                hit = pattern[p] == '?' || lower(pattern[p]) == lower(text[t]);
            if (hit)
            {
                p = next;
                ++t;
                continue;
            }
        }
        if (starP == std::string_view::npos)
            return false;
        p = starP + 1;
        t = ++starT;
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

int fuzzyScore(std::string_view pattern, std::string_view text)
{
    if (pattern.empty())
        return 0;
    int score = 60;
    size_t t = 0;
    size_t prev = std::string_view::npos;
    for (char pc : pattern)
    {
        size_t at = std::string_view::npos;
        for (; t < text.size(); ++t)
            if (lower(text[t]) == lower(pc))
            {
                at = t++;
                break;
            }
        if (at == std::string_view::npos)
            return 0;
        bool wordStart = at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1])) ||
                         (std::isupper(static_cast<unsigned char>(text[at])) && std::islower(static_cast<unsigned char>(text[at - 1])));
        if (prev != std::string_view::npos && at == prev + 1)
            score += 3;
        else if (prev != std::string_view::npos)
            score -= int(std::min<size_t>(at - prev - 1, 10));
        if (wordStart)
            score += 5;
        prev = at;
    }
    return std::clamp(score, 1, 99);
}

std::vector<std::string> filterCompletions(std::string_view token, const std::vector<std::string>& typeNames)
{
    constexpr std::string_view kKey = "type:";
    std::string negate = token.starts_with('!') ? "!" : "";
    std::string_view rest = token.substr(negate.size());
    std::string low = lowered(rest);
    std::vector<std::string> out;
    if (low.starts_with(kKey))
    {
        std::string_view value = rest.substr(kKey.size());
        if (isGlob(value))
            return out;
        for (const std::string& name : completion::rank(typeNames, value))
            out.push_back(negate + std::string(kKey) + name);
    }
    else if (!low.empty() && std::string_view(kKey).starts_with(low))
        out.push_back(negate + std::string(kKey));
    return out;
}

std::vector<std::string> nodeTypeNames()
{
    std::vector<std::string> names;
    for (const scene::NodeType* t : scene::types::all())
        names.push_back(t->name);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

bool typeMatches(const scene::NodeType* type, std::string_view pattern)
{
    for (const scene::NodeType* t = type; t; t = t->base)
        if (globMatch(pattern, t->name))
            return true;
    return false;
}

Filter::Filter(std::string_view text, bool regex)
{
    for (std::string& tok : split(text, !regex))
    {
        bool negate = tok.size() > 1 && tok[0] == '!';
        if (negate)
            tok.erase(0, 1);
        if (tok.size() > 5 && lowered(tok.substr(0, 5)) == "type:")
        {
            (negate ? mNotTypes : mTypes).push_back(lowered(tok.substr(5)));
            continue;
        }
        Term term;
        if (regex)
        {
            term.text = tok;
            term.onPath = tok.find("\\|") != std::string::npos;
            try
            {
                term.re = std::make_shared<std::regex>(tok, std::regex::ECMAScript | std::regex::icase);
            }
            catch (const std::regex_error& e)
            {
                mError = "Invalid regex '" + tok + "': " + e.what();
                continue;
            }
        }
        else
        {
            term.text = lowered(tok);
            term.onPath = tok.find('|') != std::string::npos;
            term.glob = isGlob(tok);
        }
        (negate ? mExclude : mInclude).push_back(std::move(term));
    }
}

bool Filter::empty() const
{
    return mInclude.empty() && mExclude.empty() && mTypes.empty() && mNotTypes.empty() && mType.empty();
}

int Filter::termScore(const Term& t, std::string_view name, std::string_view path) const
{
    std::string_view target = t.onPath ? path : name;
    if (t.re)
        return std::regex_search(target.begin(), target.end(), *t.re) ? 200 : 0;
    if (t.glob)
        return globMatch(t.text, target) ? 300 : 0;
    std::string hay = lowered(target);
    if (hay == t.text)
        return 400;
    if (hay.starts_with(t.text))
        return 250;
    if (hay.find(t.text) != std::string::npos)
        return t.onPath ? 150 : 200;
    return 0;
}

int Filter::score(std::string_view name, std::string_view fullPath, const scene::NodeType* type) const
{
    if (!valid())
        return 1;
    if (!mType.empty() && !typeMatches(type, mType))
        return 0;
    if (!mTypes.empty() && std::none_of(mTypes.begin(), mTypes.end(), [&](const std::string& p) { return typeMatches(type, p); }))
        return 0;
    for (const std::string& p : mNotTypes)
        if (typeMatches(type, p))
            return 0;
    for (const Term& t : mExclude)
        if (termScore(t, name, fullPath))
            return 0;
    if (mInclude.empty())
        return 1;
    int best = 0;
    for (const Term& t : mInclude)
    {
        int s = termScore(t, name, fullPath);
        if (!s && mFuzzy && !t.glob && !t.re)
            s = fuzzyScore(t.text, t.onPath ? fullPath : name);
        best = std::max(best, s);
    }
    return best;
}

std::vector<scene::NodeId> hierarchyNodes(const scene::Graph& g)
{
    std::vector<scene::NodeId> out;
    std::vector<scene::NodeId> stack(g.roots().rbegin(), g.roots().rend());
    while (!stack.empty())
    {
        scene::NodeId id = stack.back();
        stack.pop_back();
        if (!g.node(id))
            continue;
        out.push_back(id);
        const auto& kids = g.children(id);
        stack.insert(stack.end(), kids.rbegin(), kids.rend());
    }
    return out;
}

FilterRows filterRows(const scene::Graph& g, const Filter& f)
{
    LILA_ZONE();

    FilterRows rows;
    for (scene::NodeId id : hierarchyNodes(g))
    {
        const scene::Node* n = g.node(id);
        if (f.matches(n->name(), g.fullPath(id), &n->type()))
            rows.matched.insert(id);
    }
    for (scene::NodeId id : rows.matched)
        for (scene::NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
            if (rows.matched.contains(p) || !rows.kept.insert(p).second)
                break;
    return rows;
}
} // namespace lila::ui::outliner
