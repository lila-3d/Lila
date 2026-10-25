#pragma once

#include "core/Export.h"

#include "retopo/Construct.h"
#include "scene/Graph.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lila::retopo
{
/**
 * The topology toolkit's commands: retopoSetGuides, retopoConstruct, retopoConstructDelete, retopoConstructFreeze and
 * retopoSettle. Called from registerCommands.
 */
void registerTopologyCommands();

/** A planning line drawn with the Pencil. */
struct Guide
{
    std::vector<glm::dvec3> points;
    /** sRGB, 0..1. */
    glm::vec3 color{1.0f, 0.55f, 0.1f};
    bool closed = false;
};

/** A retopo mesh's guides, in world space. */
LILA_SCENE_API std::vector<Guide> retopoGuides(const scene::Node& retopo);

/** One live construct as stored on a retopo mesh. */
struct ConstructRecord
{
    int id = 0;
    ConstructKind kind = ConstructKind::Patch;
    /** Its spec as retopoConstruct takes it, in world space. */
    nlohmann::json spec;
    /** False once its faces were edited by hand: the next change freezes it instead of regenerating. */
    bool live = true;
    /** The counts it last generated with (see GenResult::sideCounts). */
    std::vector<int> counts;
};

/** A retopo mesh's constructs, oldest first. live is checked against the mesh's current faces. */
LILA_SCENE_API std::vector<ConstructRecord> retopoConstructs(const scene::Graph& g, const scene::Node& retopo);

/** The construct id a face of the retopo mesh's base mesh belongs to, 0 for none. */
LILA_SCENE_API int constructOfFace(const geo::Mesh& mesh, Index face);

/** Parses a spec (any space) into its typed form; throws std::runtime_error naming the bad field. */
LILA_SCENE_API ConstructSpec parseConstructSpec(ConstructKind kind, const nlohmann::json& spec);

/** A guide colour from a palette that cycles, for the nth guide. */
LILA_SCENE_API glm::vec3 guidePaletteColor(int n);
} // namespace lila::retopo
