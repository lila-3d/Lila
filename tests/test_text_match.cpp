#include <doctest/doctest.h>

#include "core/TextMatch.h"

using lila::textMatch::Query;

TEST_CASE("a search query splits on whitespace and lowercases its terms")
{
    Query q("  AO\tRadius  ");
    CHECK(q.terms() == std::vector<std::string>{"ao", "radius"});
    CHECK(Query("").empty());
    CHECK(Query(" \t ").empty());
}

TEST_CASE("an empty query matches everything")
{
    CHECK(Query("").matches({"anything"}));
    CHECK(Query("   ").matches(std::vector<std::string>{}));
}

TEST_CASE("every term must appear, case-insensitively, in some field")
{
    std::vector<std::string> fields = {"AO radius (cm)", "", "/viewport/ao/radius", "Viewport / Lighting"};
    CHECK(Query("radius").matches(fields));
    CHECK(Query("RADIUS ao").matches(fields));
    CHECK(Query("lighting radius").matches(fields));
    CHECK(Query("viewport/ao").matches(fields));
    CHECK_FALSE(Query("radius intensity").matches(fields));
    CHECK_FALSE(Query("grid").matches(fields));
}

TEST_CASE("terms match partial words and the tooltip field")
{
    CHECK(Query("multip").matches({"Custom speed multiplier", "Real-time multiplier used when Playback speed is Custom"}));
    CHECK(Query("real-time custom").matches({"Custom speed multiplier", "Real-time multiplier used when Playback speed is Custom"}));
    CHECK_FALSE(Query("speeds").matches({"Custom speed multiplier"}));
}
