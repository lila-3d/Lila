#pragma once

#include <doctest/doctest.h>

#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

namespace terraintest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

inline void loadTerrain()
{
    if (!gn::findDecl("HeightfieldGrid"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

/** The terrain's height at world (x, z), through terrainSample. */
inline double heightAt(const json& terrain, double x, double z)
{
    return run("terrainSample", {{"args", {terrain["mesh"]}}, {"points", {{x, z}}}})[0]["height"].get<double>();
}
} // namespace terraintest
