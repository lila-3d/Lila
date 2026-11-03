#pragma once

#include <RigidScene.h>

#include "ops/Command.h"

#include <string>

namespace stagehandtest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

inline physics::BodyDesc box(const glm::dvec3& at, const glm::dvec3& half, physics::Role role = physics::Role::Dynamic, double density = 600.0)
{
    physics::BodyDesc b;
    b.shape.kind = physics::Shape::Kind::Box;
    b.shape.halfExtents = half;
    b.position = at;
    b.role = role;
    b.density = density;
    return b;
}

inline std::string cube(const char* name, const glm::dvec3& at, const glm::dvec3& size)
{
    return run("polyCube", {{"name", name}, {"position", json::array({at.x, at.y, at.z})}, {"width", size.x}, {"height", size.y}, {"depth", size.z},
                            {"baseAtOrigin", true}, {"generator", false}})[0]
        .get<std::string>();
}
} // namespace stagehandtest
