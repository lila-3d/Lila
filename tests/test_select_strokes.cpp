#include "geo/Lasso.h"
#include "ui/CursorBadges.h"
#include "ui/TabSelectLogic.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace lila;

TEST_CASE("lasso contains the points inside its outline")
{
    std::vector<glm::dvec2> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    CHECK(geo::lasso::contains(square, {5, 5}));
    CHECK(geo::lasso::contains(square, {0.5, 9.5}));
    CHECK_FALSE(geo::lasso::contains(square, {-1, 5}));
    CHECK_FALSE(geo::lasso::contains(square, {5, 11}));
    CHECK_FALSE(geo::lasso::contains(square, {20, 20}));

    // A U shape: the notch between the arms is outside.
    std::vector<glm::dvec2> u{{0, 0}, {10, 0}, {10, 10}, {7, 10}, {7, 3}, {3, 3}, {3, 10}, {0, 10}};
    CHECK(geo::lasso::contains(u, {1.5, 8}));
    CHECK(geo::lasso::contains(u, {8.5, 8}));
    CHECK(geo::lasso::contains(u, {5, 1.5}));
    CHECK_FALSE(geo::lasso::contains(u, {5, 6}));
}

TEST_CASE("lasso is closed implicitly and needs three points")
{
    CHECK_FALSE(geo::lasso::contains({}, {0, 0}));
    CHECK_FALSE(geo::lasso::contains({{0, 0}, {10, 10}}, {5, 5}));
    std::vector<glm::dvec2> tri{{0, 0}, {10, 0}, {0, 10}};
    CHECK(geo::lasso::contains(tri, {2, 2}));
    CHECK_FALSE(geo::lasso::contains(tri, {8, 8}));
}

TEST_CASE("self-crossing lasso uses the even-odd rule")
{
    // A bow tie: the two lobes are inside, the area beyond the crossing is not.
    std::vector<glm::dvec2> bow{{0, 0}, {10, 10}, {10, 0}, {0, 10}};
    CHECK(geo::lasso::contains(bow, {2, 5}));
    CHECK(geo::lasso::contains(bow, {8, 5}));
    CHECK_FALSE(geo::lasso::contains(bow, {5, 1}));
}

TEST_CASE("lasso extend skips points too close to the last")
{
    std::vector<glm::dvec2> poly;
    CHECK(geo::lasso::extend(poly, {0, 0}));
    CHECK_FALSE(geo::lasso::extend(poly, {1, 1}));
    CHECK(geo::lasso::extend(poly, {3, 0}));
    CHECK(poly.size() == 2);
    auto [lo, hi] = geo::lasso::bounds({{3, -1}, {-2, 4}, {1, 1}});
    CHECK(lo == glm::dvec2(-2, -1));
    CHECK(hi == glm::dvec2(3, 4));
}

TEST_CASE("screen distance to a segment clamps to its ends")
{
    using geo::lasso::segmentDistance;
    CHECK(segmentDistance({5, 3}, {0, 0}, {10, 0}) == doctest::Approx(3.0));
    CHECK(segmentDistance({-3, 4}, {0, 0}, {10, 0}) == doctest::Approx(5.0));
    CHECK(segmentDistance({13, -4}, {0, 0}, {10, 0}) == doctest::Approx(5.0));
    CHECK(segmentDistance({1, 1}, {2, 2}, {2, 2}) == doctest::Approx(std::sqrt(2.0)));
}

TEST_CASE("a segment touches a box it crosses even with both ends outside")
{
    using geo::lasso::segmentTouchesBox;
    const glm::dvec2 lo(0, 0), hi(10, 10);
    CHECK(segmentTouchesBox({-50, 5}, {50, 5}, lo, hi));
    CHECK(segmentTouchesBox({-5, -5}, {15, 15}, lo, hi));
    CHECK(segmentTouchesBox({2, 2}, {3, 3}, lo, hi));
    CHECK(segmentTouchesBox({5, 5}, {50, 50}, lo, hi));
    CHECK_FALSE(segmentTouchesBox({-50, 20}, {50, 20}, lo, hi));
    // Its bounds overlap the box but the line passes the corner.
    CHECK_FALSE(segmentTouchesBox({-5, 8}, {8, 21}, lo, hi));
    CHECK_FALSE(segmentTouchesBox({20, 0}, {20, 10}, lo, hi));
    CHECK(geo::lasso::touches(geo::lasso::box(lo, hi), {{-50, 5}, {50, 5}}));
}

TEST_CASE("tab select mode is decided by what is under the press")
{
    using ui::tabSelect::Mode;
    using ui::tabSelect::modeAtPress;
    CHECK(modeAtPress(false, false, false) == Mode::Select);
    CHECK(modeAtPress(true, false, false) == Mode::Deselect);
    CHECK(modeAtPress(std::nullopt, false, false) == Mode::Select);
    CHECK(modeAtPress(true, true, false) == Mode::Deselect);
    CHECK(modeAtPress(false, true, false) == Mode::Select);
    // Ctrl removes and Ctrl+Shift adds whatever is under the press.
    CHECK(modeAtPress(false, false, true) == Mode::Deselect);
    CHECK(modeAtPress(std::nullopt, false, true) == Mode::Deselect);
    CHECK(modeAtPress(true, true, true) == Mode::Select);
}

TEST_CASE("tab select badges follow the stroke mode")
{
    ui::cursorBadges::registerDefaults();
    auto shown = [](const ui::ViewportState& st, const char* id) {
        std::vector<const ui::cursorBadges::Badge*> on = ui::cursorBadges::active(st);
        return std::any_of(on.begin(), on.end(), [&](const ui::cursorBadges::Badge* b) { return b->id == id; });
    };
    ui::ViewportState st;
    CHECK_FALSE(shown(st, "tabSelect.add"));
    CHECK_FALSE(shown(st, "tabSelect.remove"));
    st.tabSelects = true;
    CHECK(shown(st, "tabSelect.add"));
    CHECK_FALSE(shown(st, "tabSelect.remove"));
    st.tabSelects = false;
    CHECK(shown(st, "tabSelect.remove"));
    CHECK_FALSE(shown(st, "tabSelect.add"));
}
