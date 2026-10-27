#pragma once

#include "core/Export.h"

#include "geo/FoldEdges.h"
#include "scene/Components.h"
#include "scene/Node.h"

#include <optional>
#include <vector>

namespace lila::geo
{
struct MeshRemap;
}

namespace lila::scene
{
class Graph;

/**
 * The faces or edges an extrudeModifier extrudes, remembered well enough to find them again after the mesh under it
 * changes: by id while the topology is the one they were taken on (the capacities), else faces by centre and edges by
 * geo::EdgeMatcher. Components found nowhere are dropped.
 */
struct LILA_SCENE_API ExtrudeComponents final : Data
{
    struct FaceRef
    {
        geo::Index face = geo::kInvalid;
        glm::dvec3 centre{0.0};
    };

    bool edges = false;
    std::vector<FaceRef> faces;
    std::vector<geo::EdgeRef> edgeRefs;
    geo::Index pointCount = 0;
    geo::Index edgeCount = 0;
    geo::Index faceCount = 0;

    const char* kind() const override { return "extrudeComponents"; }
    json toJson() const override;
    size_t bytes() const override { return sizeof(*this) + faces.size() * sizeof(FaceRef) + edgeRefs.size() * sizeof(geo::EdgeRef); }
    static DataPtr fromJson(const json& j);
    /** Remembers ids (faces, or edges when `edges`) on `mesh`. */
    static std::shared_ptr<ExtrudeComponents> take(const geo::Mesh& mesh, bool edges, const std::vector<geo::Index>& ids);
};

namespace extrudeModifier
{
LILA_SCENE_API bool isExtrude(const Node& n);

/** What a modifier extrudes on its current input. No stored components means every face. */
struct Resolved
{
    MeshPtr input;
    ComponentMode kind = ComponentMode::Face;
    std::vector<geo::Index> ids;
    /** Stored components found nowhere on the input. */
    int lost = 0;
};
/** Evaluates the graph. Empty when the modifier has no input. */
LILA_SCENE_API std::optional<Resolved> resolve(Graph& g, NodeId modifier);

/** Finds stored components on m; none stored means every face. */
LILA_SCENE_API Resolved resolveComponents(const geo::Mesh& m, const ExtrudeComponents* c);

/** The mesh's selected faces, or edges in edge mode, with their mirror images under symmetry; null in object mode or with none. */
LILA_SCENE_API DataPtr selectedComponents(Graph& g, NodeId mesh);

/** c moved to base's ids after a compaction, or null when c was taken on other topology. */
LILA_SCENE_API DataPtr remapComponents(const ExtrudeComponents* c, const geo::MeshRemap& remap, const geo::Mesh& base);

/** A direct edit compacted the base mesh: the components of an extrudeModifier first in the stack follow to the new ids. */
LILA_SCENE_API void baseRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap, const geo::Mesh& base);

LILA_SCENE_API void registerTypes();
} // namespace extrudeModifier
} // namespace lila::scene
