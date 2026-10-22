#include <doctest/doctest.h>

#include "ui/NumberExpr.h"

TEST_CASE("boolean channel text")
{
    using lila::ui::parseBool;
    for (const char* on : {"on", "ON", " 1 ", "true", "yes"})
        CHECK(parseBool(on) == true);
    for (const char* off : {"off", "0", "False", "no"})
        CHECK(parseBool(off) == false);
    CHECK_FALSE(parseBool("maybe").has_value());
    CHECK_FALSE(parseBool("").has_value());
}
