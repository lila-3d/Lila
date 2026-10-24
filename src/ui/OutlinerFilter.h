#pragma once

#include "core/Export.h"

#include "scene/Graph.h"

#include <memory>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ui::outliner
{
/**
 * ASCII case-insensitive glob over the whole text: `*` any run, `?` one character, `[abc]` `[a-z]` a class and
 * `[!a-z]` or `[^a-z]` its complement. An unclosed `[` is a literal.
 */
LILA_UI_API bool globMatch(std::string_view pattern, std::string_view text);

/** pattern's characters appear in text in order (case-insensitive); higher for tighter runs and word starts. 0 when not. */
LILA_UI_API int fuzzyScore(std::string_view pattern, std::string_view text);

/**
 * The outliner's filter text. Terms are split on spaces and commas (spaces only in regex mode) and any of them may
 * match. A term with `* ? [` is a glob over the whole name, otherwise a substring of it; a term containing `|`
 * matches the full DAG path instead. `!term` excludes what it matches. `type:mesh` keeps nodes of that type or a type
 * derived from it (globs allowed, several are alternatives), `!type:` excludes. Matching ignores case. In regex mode
 * each term is an ECMAScript regex searched in the name, or in the full path when it contains `\|`.
 */
class LILA_UI_API Filter
{
public:
    Filter() = default;
    explicit Filter(std::string_view text, bool regex = false);

    /** Also accept names holding a plain term's letters in order, ranked below real matches (the scene finder). */
    void setFuzzy(bool fuzzy) { mFuzzy = fuzzy; }
    /** Keeps only nodes of this type or one derived from it, on top of any `type:` terms. Empty for any type. */
    void setType(std::string type) { mType = std::move(type); }

    /** Nothing to filter by: every node passes. */
    bool empty() const;
    /** False for a regex that does not compile; such a filter passes every node. */
    bool valid() const { return mError.empty(); }
    const std::string& error() const { return mError; }

    /** 0 when the node is filtered out, else a rank: whole-name matches over prefixes over substrings over fuzzy. */
    int score(std::string_view name, std::string_view fullPath, const scene::NodeType* type) const;
    bool matches(std::string_view name, std::string_view fullPath, const scene::NodeType* type) const
    {
        return score(name, fullPath, type) > 0;
    }

private:
    struct Term
    {
        std::string text; // lowercased
        bool onPath = false;
        bool glob = false;
        std::shared_ptr<std::regex> re;
    };
    int termScore(const Term& t, std::string_view name, std::string_view path) const;

    std::vector<Term> mInclude;
    std::vector<Term> mExclude;
    std::vector<std::string> mTypes;
    std::vector<std::string> mNotTypes;
    std::string mType;
    std::string mError;
    bool mFuzzy = false;
};

/** True when type, or one of its base types, is named by pattern (a glob, case-insensitive). */
LILA_UI_API bool typeMatches(const scene::NodeType* type, std::string_view pattern);

/** Every node the outliner lists (the hierarchy from the roots down), parents before children. */
LILA_UI_API std::vector<scene::NodeId> hierarchyNodes(const scene::Graph& g);

struct FilterRows
{
    std::set<scene::NodeId> matched;
    /** Ancestors of matched nodes that do not match themselves, kept so the hierarchy reads. */
    std::set<scene::NodeId> kept;
};
LILA_UI_API FilterRows filterRows(const scene::Graph& g, const Filter& f);
} // namespace lila::ui::outliner
