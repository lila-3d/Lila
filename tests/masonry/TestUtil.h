#pragma once

#include "MasonryCore.h"
#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <array>
#include <initializer_list>

namespace masonrytest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

inline masonry::Footprint line(std::initializer_list<glm::dvec3> pts, bool closed = false) { return {std::vector<glm::dvec3>(pts), closed, {}}; }

inline json pts(std::initializer_list<std::array<double, 3>> p)
{
    json out = json::array();
    for (const auto& q : p)
        out.push_back({q[0], q[1], q[2]});
    return out;
}

inline void loadMasonry()
{
    if (!gn::findDecl("MasonryWall"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_MASONRY_PACKAGE) == "Masonry");
}
} // namespace masonrytest
