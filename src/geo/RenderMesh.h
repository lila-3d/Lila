#pragma once

#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Triangulate.h"

#include <glm/glm.hpp>

#include <vector>

namespace lila::geo
{
struct RenderMeshOptions
{
    int maxUvSets = 8;
    bool tangents = false;
    bool colors = true;
    /** Corner float4 layer the colors come from; empty is the mesh's current color set. */
    std::string colorLayer;
    bool wireframe = true;
    /** Group triangles into one section per face "material" id. */
    bool sections = true;
    /** Used when the mesh has no corner "normal" attribute. */
    CornerNormalOptions normals;
    TriangulateOptions triangulation;
};

struct RenderSection
{
    int32_t material = 0;
    Index firstIndex = 0;
    Index indexCount = 0;
};

/**
 * Flat GPU-ready buffers. One vertex per unique combination of point and corner attributes, so
 * vertices split exactly along hard edges, uv seams and colour borders.
 */
struct RenderMesh
{
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;            // empty unless requested and uv0 exists
    std::vector<glm::vec4> colors;              // empty unless the mesh has a color set
    std::vector<std::vector<glm::vec2>> uvs;    // uv0, uv1, ... as present
    std::vector<Index> vertexPoint;             // source point per vertex
    std::vector<Index> vertexCorner;            // one source corner per vertex
    std::vector<Index> cornerVertex;            // vertex per source corner, kInvalid for dead corners
    std::vector<Index> indices;                 // 3 per triangle, ordered by section
    std::vector<Index> triangleFaces;           // source face per triangle
    std::vector<RenderSection> sections;
    std::vector<Index> lineIndices;             // 2 per edge, into the vertex buffers
    std::vector<Index> lineEdges;               // source edge per line

    Index vertexCount() const { return Index(positions.size()); }
    Index triangleCount() const { return Index(indices.size() / 3); }
};

/** Parallel. */
RenderMesh buildRenderMesh(const Mesh& mesh, const RenderMeshOptions& opts = {});
} // namespace lila::geo
