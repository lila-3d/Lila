#pragma once

#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <string>

namespace terrain
{
/** The Terrain Regions node and terrainQuery. */
bool registerQuery(lila::ext::Context& ctx);

/**
 * Lets the point, path and polygon flags of other terrain commands take region references ('region:<name>' or
 * 'region:<name>/<k>'). Call after every terrain command is registered.
 */
void registerRegionRefs(lila::ext::Context& ctx);

/** The named regions stored on a terrain: {name: {rings, point, query, ...}} in the heightfield's own space. */
nlohmann::json storedRegions(const std::string& terrain);

/**
 * Replaces the stored regions whose names start with prefix by regions ({name: {rings, parts, point, query, ...}} in the
 * heightfield's own space), in one edit of the terrain's group. Throws CommandError naming cmd.
 */
void replaceRegions(const std::string& terrain, const std::string& prefix, const nlohmann::json& regions, const std::string& cmd);

/** A region reference as a ground point [x, z] (polygon false) or ring [[x, z], ...] (polygon true), world space. Throws CommandError. */
nlohmann::json resolveRegionRef(const std::string& terrain, const std::string& ref, bool polygon, const std::string& cmd);
} // namespace terrain
