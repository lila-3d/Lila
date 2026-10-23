#include "core/TextMatch.h"

#include <algorithm>

namespace lila::textMatch
{
namespace
{
char lowerChar(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

bool containsLower(std::string_view haystack, std::string_view loweredNeedle)
{
    auto it = std::search(haystack.begin(), haystack.end(), loweredNeedle.begin(), loweredNeedle.end(),
                          [](char a, char b) { return lowerChar(a) == b; });
    return it != haystack.end();
}

template <typename Fields>
bool matchAll(const std::vector<std::string>& terms, const Fields& fields)
{
    return std::all_of(terms.begin(), terms.end(), [&](const std::string& term) {
        return std::any_of(std::begin(fields), std::end(fields), [&](std::string_view f) { return containsLower(f, term); });
    });
}
} // namespace

std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lowerChar);
    return out;
}

Query::Query(std::string_view text)
{
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && isSpace(text[i]))
            ++i;
        size_t start = i;
        while (i < text.size() && !isSpace(text[i]))
            ++i;
        if (i > start)
            mTerms.push_back(lower(text.substr(start, i - start)));
    }
}

bool Query::matches(std::initializer_list<std::string_view> fields) const { return matchAll(mTerms, fields); }

bool Query::matches(const std::vector<std::string>& fields) const { return matchAll(mTerms, fields); }
} // namespace lila::textMatch
