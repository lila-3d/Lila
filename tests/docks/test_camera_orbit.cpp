#include <doctest/doctest.h>

#include "ui/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila::ui;

TEST_CASE("tumbling about a point keeps that point where it was on screen")
{
    Camera cam;
    cam.target = glm::dvec3(0.0);
    cam.distance = 200.0;
    const glm::dvec3 pivot(30.0, 10.0, -20.0);
    const glm::dvec3 before = glm::dvec3(cam.view() * glm::dvec4(pivot, 1.0));
    cam.tumbleAbout(pivot, 25.0, -12.0);
    const glm::dvec3 after = glm::dvec3(cam.view() * glm::dvec4(pivot, 1.0));
    CHECK(glm::length(after - before) < 1e-6);
    // The view really turned.
    Camera plain;
    plain.target = glm::dvec3(0.0);
    plain.distance = 200.0;
    CHECK(glm::length(cam.forward() - plain.forward()) > 0.1);
}
