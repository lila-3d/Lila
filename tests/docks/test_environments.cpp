#include <doctest/doctest.h>

#include "ui/Environments.h"

using namespace lila::ui;

TEST_CASE("environments: built-in HDRIs get readable menu names")
{
    CHECK(environments::displayName("studio_small_09_2k.hdr") == "Studio Small 09");
    CHECK(environments::displayName("kloofendal_48d_partly_cloudy_puresky_2k.hdr") == "Kloofendal 48d Partly Cloudy (Pure Sky)");
    CHECK(environments::displayName("my_sky.hdr") == "My Sky");
    CHECK(environments::displayName("Office.hdr") == "Office");

    auto shipped = environments::builtIn();
    CHECK(shipped.size() >= 9);
    for (const auto& hdri : shipped)
        CHECK_FALSE(environments::displayName(hdri).empty());
}
