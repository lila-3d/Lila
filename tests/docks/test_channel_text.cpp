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

TEST_CASE("typed number math")
{
    using lila::ui::evalExpression;
    CHECK(evalExpression("*=3", 2048.0) == doctest::Approx(6144.0));
    CHECK(evalExpression("/=2", 10.0) == doctest::Approx(5.0));
    CHECK(evalExpression("+=2*3", 1.0) == doctest::Approx(7.0));
    CHECK(evalExpression("-=1", 1.0) == doctest::Approx(0.0));
    CHECK(evalExpression("2048/4", 0.0) == doctest::Approx(512.0));
    CHECK(evalExpression("=(1+2)*4", 0.0) == doctest::Approx(12.0));
    CHECK(evalExpression("-5", 3.0) == doctest::Approx(-5.0));
    CHECK_FALSE(evalExpression("/=0", 1.0).has_value());
    CHECK_FALSE(evalExpression("abc", 1.0).has_value());
    CHECK_FALSE(evalExpression("", 1.0).has_value());
}
