#pragma once

#include "core/Export.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace lila::textMatch
{
/**
 * A search box query: whitespace separated terms, all of which must appear (ASCII case-insensitive) somewhere in the
 * searched text. An empty or all-whitespace query matches everything.
 */
class LILA_FOUNDATION_API Query
{
public:
    explicit Query(std::string_view text);

    bool empty() const { return mTerms.empty(); }
    const std::vector<std::string>& terms() const { return mTerms; }

    /** True when every term occurs in at least one of the fields. */
    bool matches(std::initializer_list<std::string_view> fields) const;
    bool matches(const std::vector<std::string>& fields) const;

private:
    std::vector<std::string> mTerms;
};

/** ASCII lowercase copy. */
LILA_FOUNDATION_API std::string lower(std::string_view s);
} // namespace lila::textMatch
