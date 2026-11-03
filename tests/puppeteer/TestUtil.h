#pragma once

#include <doctest/doctest.h>

#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "ui/Gizmo.h"

#include <glm/gtc/matrix_transform.hpp>

#include <QPointF>

#include <cmath>
#include <string>

namespace puptest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
inline json v3(double x, double y, double z) { return json::array({x, y, z}); }

inline void loadPuppeteer()
{
    if (!ops::find("puppeteerIkChain"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_PUPPETEER_PACKAGE) == "Puppeteer");
}

inline scene::Graph& graph() { return scene::Graph::current(); }

inline scene::NodeId id(const std::string& name)
{
    scene::Node* n = graph().find(name);
    REQUIRE(n);
    return n->id();
}

inline glm::dmat4 world(const std::string& name)
{
    graph().evaluate();
    return std::get<glm::dmat4>(graph().value({id(name), scene::Transform::kWorldMatrix, -1}));
}

inline glm::dvec3 pos(const std::string& name) { return glm::dvec3(world(name)[3]); }
inline double attr(const std::string& plug) { return std::get<double>(graph().value(graph().plug(plug))); }

inline bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-9) { return glm::length(a - b) <= eps; }

inline bool near(const glm::dmat4& a, const glm::dmat4& b, double eps = 1e-9)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > eps)
                return false;
    return true;
}

/**
 * The Spinner sits 250 px right of the view's centre by default: (650, 300) in the 800 x 600 view. Its layout is
 * measured on a 424 px canvas, drawn at radius 120 px, so a canvas length L is L * 120 / 212 pixels.
 */
inline const QPointF kCenter(650.0, 300.0);

/** An 800 x 600 perspective view looking down -Z at the origin, as a viewport hands its gizmo. */
inline ui::ViewInfo view(const char* camera = "persp")
{
    ui::ViewInfo v;
    v.eye = {0.0, 0.0, 300.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(0.0), glm::dvec3(0, 1, 0));
    v.proj = glm::perspective(glm::radians(45.0), 800.0 / 600.0, 1.0, 10000.0);
    v.proj[1][1] *= -1.0; // y down on screen, as the viewport's projection
    v.width = 800.0;
    v.height = 600.0;
    v.camera = camera;
    return v;
}

inline double u(double canvas) { return canvas * 120.0 / 212.0; }
} // namespace puptest
