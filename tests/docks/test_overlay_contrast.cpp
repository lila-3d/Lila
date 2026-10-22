#include "ui/Overlay.h"

#include <doctest/doctest.h>

using lila::ui::OverlayPainter;

TEST_CASE("overlay digits are tabular so counts never shift")
{
    CHECK(OverlayPainter::textWidth("1111111") == doctest::Approx(OverlayPainter::textWidth("8888888")));
    CHECK(OverlayPainter::textWidth("0") == doctest::Approx(OverlayPainter::textWidth("7")));
}

TEST_CASE("backdrop classification switches with hysteresis")
{
    std::vector<float> state;
    OverlayPainter::classifyBackdrop({0.05f, 0.5f, 0.19f}, state);
    CHECK(state == std::vector<float>{0.0f, 1.0f, 1.0f});

    // Small moves around the switch point keep the previous answer.
    OverlayPainter::classifyBackdrop({0.2f, 0.16f, 0.17f}, state);
    CHECK(state == std::vector<float>{0.0f, 1.0f, 1.0f});

    // Clear moves flip it.
    OverlayPainter::classifyBackdrop({0.3f, 0.1f, 0.12f}, state);
    CHECK(state == std::vector<float>{1.0f, 0.0f, 0.0f});
}

TEST_CASE("ink turns dark over a bright backdrop, per region")
{
    std::vector<lila::render::OverlayVertex> out;
    OverlayPainter p(out, 1.0);
    CHECK(p.ink(0.85f).r == doctest::Approx(0.85f)); // no backdrop counts as dark

    // Left half dark, right half bright.
    p.setBackdrop({0.0f, 1.0f, 0.0f, 1.0f}, 2, 2, QSizeF(200, 100));
    CHECK_FALSE(p.brightBackdrop(QRectF(10, 10, 50, 20)));
    CHECK(p.brightBackdrop(QRectF(120, 60, 50, 20)));
    CHECK(p.ink(0.85f, 1.0f, QRectF(10, 10, 50, 20)).r == doctest::Approx(0.85f));
    glm::vec4 dark = p.ink(0.85f, 0.9f, QRectF(120, 60, 50, 20));
    CHECK(dark.r < 0.1f);
    CHECK(dark.a == doctest::Approx(0.9f));
    // Dim chrome stays well clear of a mid-gray backdrop.
    CHECK(p.ink(0.45f, 1.0f, QRectF(120, 60, 50, 20)).r < 0.25f);
}
