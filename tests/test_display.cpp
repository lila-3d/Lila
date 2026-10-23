#include <doctest/doctest.h>

#include "render/DisplayOptions.h"
#include "ui/ViewportDisplay.h"

using namespace lila::render;

TEST_CASE("viewport MSAA sample count")
{
    constexpr uint32_t upTo8 = 1 | 2 | 4 | 8;
    CHECK(msaaSamples(false, 8, upTo8) == 1);
    CHECK(msaaSamples(true, 8, upTo8) == 8);
    CHECK(msaaSamples(true, 4, upTo8) == 4);
    CHECK(msaaSamples(true, 3, upTo8) == 2);
    CHECK(msaaSamples(true, 0, upTo8) == 1);
    CHECK(msaaSamples(true, 64, upTo8) == 8);
    SUBCASE("rounds down to what the device supports")
    {
        CHECK(msaaSamples(true, 8, 1 | 4) == 4);
        CHECK(msaaSamples(true, 2, 1 | 4) == 1);
        CHECK(msaaSamples(true, 8, 1) == 1);
    }
}

TEST_CASE("viewport lighting and exposure prefs")
{
    CHECK(lightingFromName("studio") == FrameState::Lighting::Studio);
    CHECK(lightingFromName("flat") == FrameState::Lighting::Flat);
    CHECK(lightingFromName("environment") == FrameState::Lighting::Environment);
    CHECK(lightingFromName("bogus") == FrameState::Lighting::Environment);
    CHECK(exposureScale(0.0) == doctest::Approx(1.0));
    CHECK(exposureScale(1.0) == doctest::Approx(2.0));
    CHECK(exposureScale(-2.0) == doctest::Approx(0.25));
}

TEST_CASE("environment intensity per HDRI")
{
    nlohmann::json byHdri = nlohmann::json::object();
    CHECK(environmentIntensity(byHdri, "a.hdr", 1.5f) == doctest::Approx(1.5f));
    CHECK(environmentIntensity(nullptr, "a.hdr", 0.8f) == doctest::Approx(0.8f));

    byHdri = withEnvironmentIntensity(byHdri, "a.hdr", 2.0f);
    byHdri = withEnvironmentIntensity(byHdri, "C:/hdri/b.hdr", 0.5f);
    CHECK(environmentIntensity(byHdri, "a.hdr", 1.0f) == doctest::Approx(2.0f));
    CHECK(environmentIntensity(byHdri, "C:/hdri/b.hdr", 1.0f) == doctest::Approx(0.5f));
    CHECK(environmentIntensity(byHdri, "c.hdr", 1.25f) == doctest::Approx(1.25f));

    SUBCASE("clearing an entry falls back to the global value")
    {
        byHdri = withEnvironmentIntensity(byHdri, "a.hdr", std::nullopt);
        CHECK(environmentIntensity(byHdri, "a.hdr", 1.25f) == doctest::Approx(1.25f));
        CHECK(byHdri.contains("C:/hdri/b.hdr"));
    }
    SUBCASE("bad values")
    {
        byHdri["bad"] = "bright";
        CHECK(environmentIntensity(byHdri, "bad", 1.0f) == doctest::Approx(1.0f));
        CHECK(environmentIntensity(byHdri, "a.hdr", -3.0f) == doctest::Approx(2.0f));
        CHECK(environmentIntensity(withEnvironmentIntensity(byHdri, "a.hdr", -1.0f), "a.hdr", 1.0f) == doctest::Approx(0.0f));
        CHECK(environmentIntensity(withEnvironmentIntensity(nlohmann::json(3), "a.hdr", 2.0f), "a.hdr", 1.0f) == doctest::Approx(2.0f));
    }
}

TEST_CASE("monotone overrides the selective accent colours")
{
    using lila::ui::viewportDisplay::accentColors;
    CHECK(accentColors(false, true));
    CHECK_FALSE(accentColors(false, false));
    CHECK_FALSE(accentColors(true, true));
    CHECK_FALSE(accentColors(true, false));
}
