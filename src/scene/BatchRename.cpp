#include "scene/BatchRename.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/Naming.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

namespace lila::scene
{
namespace
{
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
char upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

std::string expand(std::string_view text, int number, std::string_view name, std::string_view parent, std::string_view type)
{
    std::string out;
    for (size_t i = 0; i < text.size();)
    {
        if (text[i] == '#')
        {
            size_t run = 0;
            while (i + run < text.size() && text[i + run] == '#')
                ++run;
            std::string digits = std::to_string(std::abs(number));
            if (digits.size() < run)
                digits.insert(0, run - digits.size(), '0');
            out += (number < 0 ? "-" : "") + digits;
            i += run;
            continue;
        }
        bool token = false;
        for (auto [key, value] : {std::pair{std::string_view("{name}"), name}, {"{parent}", parent}, {"{type}", type}})
            if (text.substr(i, key.size()) == key)
            {
                out += value;
                i += key.size();
                token = true;
                break;
            }
        if (!token)
            out += text[i++];
    }
    return out;
}

std::string replacePlain(const std::string& s, const std::string& find, const std::string& with, bool matchCase)
{
    auto eq = [matchCase](char a, char b) { return matchCase ? a == b : lower(a) == lower(b); };
    std::string out;
    size_t i = 0;
    while (i < s.size())
    {
        auto it = std::search(s.begin() + long(i), s.end(), find.begin(), find.end(), eq);
        if (it == s.end())
            break;
        const size_t at = size_t(it - s.begin());
        out += s.substr(i, at - i) + with;
        i = at + find.size();
    }
    return out + s.substr(std::min(i, s.size()));
}

std::string shortOf(const std::string& name)
{
    const size_t colon = name.rfind(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

std::vector<NodeId> outlinerOrder(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId r : g.roots())
    {
        out.push_back(r);
        for (NodeId d : g.descendants(r))
            out.push_back(d);
    }
    return out;
}
} // namespace

std::string batchRenamed(const BatchRename& r, std::string_view shortName, int index, std::string_view parent, std::string_view type)
{
    const int number = r.start + index * r.step;
    auto tokens = [&](const std::string& text) { return expand(text, number, shortName, parent, type); };
    std::string s = r.name.empty() ? std::string(shortName) : tokens(r.name);
    if (!r.find.empty())
    {
        if (r.regex)
        {
            auto flags = std::regex::ECMAScript | (r.matchCase ? std::regex::flag_type{} : std::regex::icase);
            s = std::regex_replace(s, std::regex(r.find, flags), tokens(r.replace));
        }
        else
            s = replacePlain(s, r.find, tokens(r.replace), r.matchCase);
    }
    if (r.stripStart > 0)
        s.erase(0, std::min(s.size(), size_t(r.stripStart)));
    if (r.stripDigits)
    {
        size_t end = s.size();
        while (end > 0 && isDigit(s[end - 1]))
            --end;
        if (end < s.size())
        {
            while (end > 0 && (s[end - 1] == '_' || s[end - 1] == '-' || s[end - 1] == ' '))
                --end;
            s.resize(end);
        }
    }
    if (r.stripEnd > 0)
        s.resize(s.size() - std::min(s.size(), size_t(r.stripEnd)));
    if (r.caseStyle == "lower")
        std::ranges::transform(s, s.begin(), lower);
    else if (r.caseStyle == "UPPER")
        std::ranges::transform(s, s.begin(), upper);
    else if (!r.caseStyle.empty())
        s = applyCase(s, parseCaseStyle(r.caseStyle), r.underscores);
    return tokens(r.prefix) + s + tokens(r.suffix);
}

std::string sanitizeName(std::string_view s)
{
    std::string out;
    for (char c : s)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || isDigit(c);
        out += ok ? c : '_';
    }
    if (!out.empty() && isDigit(out[0]))
        out.insert(out.begin(), '_');
    return out;
}

void registerBatchRenameCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "batchRename",
        .doc = "Rename many objects at once: a new name, find/replace (plain or regex, $1 for groups), strip, case, prefix and suffix, in that "
               "order. In name, replace, prefix and suffix '#' runs number the objects (## is 01, 02...), and {name}, {parent} and "
               "{type} insert the original name, the parent's name and the node type. Defaults to the selection. Returns "
               "[{path, from, to, status}], status one of renamed, unchanged, reference, empty, clash (Lila numbers it).",
        .positionalDoc = "objects",
        .flags = {{"name", "", T::String, "", "Replaces each whole name, e.g. tail_## for tail_01, tail_02..."},
                  {"find", "", T::String, "", "Text or pattern to find; empty skips find/replace."},
                  {"replace", "", T::String, "", "What each match becomes."},
                  {"regex", "", T::Bool, false, "find is an ECMAScript regular expression."},
                  {"matchCase", "", T::Bool, false, "find is case-sensitive."},
                  {"stripStart", "", T::Int, 0, "Characters to drop from the front.", {}, {}, 0.0},
                  {"stripEnd", "", T::Int, 0, "Characters to drop from the back.", {}, {}, 0.0},
                  {"stripDigits", "", T::Bool, false, "Drop the trailing number and the separator before it."},
                  {"case", "", T::String, "", "PascalCase, camelCase, snake_case, UPPER_SNAKE, lower or UPPER; empty keeps it."},
                  {"prefix", "", T::String, "", "Added in front."},
                  {"suffix", "", T::String, "", "Added at the end."},
                  {"start", "", T::Int, 1, "The number '#' gives the first object.", {}, {}, 0.0},
                  {"step", "", T::Int, 1, "How much '#' goes up per object."},
                  {"hierarchy", "", T::Bool, false, "Include every descendant of the objects."},
                  {"order", "", T::String, "selection", "Numbering order: selection, or outliner (top to bottom)."},
                  {"preview", "", T::Bool, false, "Report what would change without renaming."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            BatchRename r{.name = a.get<std::string>("name"),
                          .find = a.get<std::string>("find"),
                          .replace = a.get<std::string>("replace"),
                          .regex = a.get<bool>("regex"),
                          .matchCase = a.get<bool>("matchCase"),
                          .stripStart = a.get<int>("stripStart"),
                          .stripEnd = a.get<int>("stripEnd"),
                          .stripDigits = a.get<bool>("stripDigits"),
                          .caseStyle = a.get<std::string>("case"),
                          .prefix = a.get<std::string>("prefix"),
                          .suffix = a.get<std::string>("suffix"),
                          .start = a.get<int>("start"),
                          .step = a.get<int>("step")};
            const std::string order = a.get<std::string>("order");
            if (order != "selection" && order != "outliner")
                throw CommandError("batchRename: order must be selection or outliner");

            std::vector<NodeId> ids;
            std::set<NodeId> seen;
            for (const std::string& path : a.objects())
            {
                Node* n = g.find(path);
                if (!n)
                    throw CommandError("batchRename: no object '" + path + "'");
                if (seen.insert(n->id()).second)
                    ids.push_back(n->id());
                if (a.get<bool>("hierarchy"))
                    for (NodeId d : g.descendants(n->id()))
                        if (seen.insert(d).second)
                            ids.push_back(d);
            }
            if (order == "outliner")
            {
                std::map<NodeId, size_t> rank;
                for (NodeId id : outlinerOrder(g))
                    rank.emplace(id, rank.size());
                std::ranges::stable_sort(ids, {}, [&](NodeId id) { return rank.contains(id) ? rank[id] : rank.size(); });
            }

            struct Plan
            {
                NodeId id;
                std::string from, to, status;
            };
            std::vector<Plan> plans;
            for (size_t i = 0; i < ids.size(); ++i)
            {
                Node& n = *g.node(ids[i]);
                const NodeId p = g.parent(n.id());
                const std::string parent = p ? shortOf(g.node(p)->name()) : std::string();
                std::string to;
                try
                {
                    r.underscores = g.namingPolicy().underscoresFor(n.type());
                    to = sanitizeName(batchRenamed(r, shortOf(n.name()), int(i), parent, n.type().name));
                }
                catch (const std::regex_error& e)
                {
                    throw CommandError(std::string("batchRename: bad pattern: ") + e.what());
                }
                if (!n.nameSpace().empty())
                    to = n.nameSpace() + ":" + to;
                const std::string status = n.referenceId() ? "reference" : shortOf(to).empty() ? "empty" : to == n.name() ? "unchanged" : "renamed";
                plans.push_back({n.id(), n.name(), status == "renamed" ? to : n.name(), status});
            }

            // A clash is a name another object keeps, or one two plans share, among the same siblings (or anywhere with unique names).
            const bool global = g.uniqueNames();
            auto scope = [&](NodeId id) { return global ? kNoNode : g.parent(id); };
            std::set<NodeId> moving;
            std::map<std::pair<NodeId, std::string>, int> claimed;
            for (const Plan& p : plans)
                if (p.status == "renamed")
                {
                    moving.insert(p.id);
                    ++claimed[{scope(p.id), p.to}];
                }
            for (NodeId id : g.nodes())
                if (!moving.contains(id))
                    if (auto it = claimed.find({scope(id), g.node(id)->name()}); it != claimed.end())
                        ++it->second;
            for (Plan& p : plans)
                if (p.status == "renamed" && claimed[{scope(p.id), p.to}] > 1)
                    p.status = "clash";

            if (!a.get<bool>("preview"))
            {
                // Everything moves to a free name first, so swaps and shifted numbering land on the names they asked for.
                int temp = 0;
                for (const Plan& p : plans)
                    if (p.status == "renamed" || p.status == "clash")
                        materials::rename(g, p.id, "lilaBatchRename" + std::to_string(++temp));
                for (Plan& p : plans)
                    if (p.status == "renamed" || p.status == "clash")
                    {
                        try
                        {
                            p.to = materials::rename(g, p.id, p.to);
                        }
                        catch (const std::invalid_argument& e)
                        {
                            throw CommandError(std::string("batchRename: ") + e.what());
                        }
                    }
            }

            json out = json::array();
            for (const Plan& p : plans)
                out.push_back({{"path", g.path(p.id)}, {"from", p.from}, {"to", p.to}, {"status", p.status}});
            return out;
        },
    });
}
} // namespace lila::scene
