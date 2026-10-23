#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::gn
{
enum class SocketType : uint8_t
{
    Geometry,
    Float,
    Int,
    Bool,
    Vector,
    Rotation, // Euler XYZ, radians
    String,
};

LILA_SCENE_API const char* socketTypeName(SocketType t);
LILA_SCENE_API bool parseSocketType(std::string_view name, SocketType* out);
LILA_SCENE_API bool isDataType(SocketType t);

/** Domains a field can be evaluated on. Mesh and point-cloud data use the first four. */
enum class Domain : uint8_t
{
    Point,
    Edge,
    Face,
    Corner,
    Curve,
    Instance,
};

LILA_SCENE_API const char* domainName(Domain d);
LILA_SCENE_API bool parseDomain(std::string_view name, Domain* out);

/** Poly curves: curve c owns points [offsets[c], offsets[c + 1]). */
struct LILA_SCENE_API CurveSet
{
    std::vector<glm::dvec3> positions;
    std::vector<geo::Index> offsets{0};
    std::vector<uint8_t> cyclic;
    geo::AttributeSet pointAttrs;
    geo::AttributeSet curveAttrs;

    size_t numCurves() const { return cyclic.size(); }
    size_t numPoints() const { return positions.size(); }
    size_t curveSize(size_t c) const { return offsets[c + 1] - offsets[c]; }
    std::span<const glm::dvec3> curvePoints(size_t c) const { return {positions.data() + offsets[c], curveSize(c)}; }

    void addCurve(std::span<const glm::dvec3> points, bool closed);
    /** Merges attribute layers by name, like Mesh::append. */
    void append(const CurveSet& other);
    /** Brings attribute sizes in line after positions or curves were added directly. */
    void syncSizes();
};

struct Geometry;
using GeometryPtr = std::shared_ptr<const Geometry>;

/** Transformed references to other geometry, one per instance. */
struct LILA_SCENE_API Instances
{
    std::vector<glm::dmat4> transforms;
    std::vector<int> reference; // index into references
    std::vector<GeometryPtr> references;
    geo::AttributeSet attrs;

    size_t size() const { return transforms.size(); }
    void add(const glm::dmat4& xf, int ref);
};

/**
 * What flows along geometry sockets: up to one of each component, shared and immutable. A node that changes one
 * copies just that component (copy on write).
 */
struct LILA_SCENE_API Geometry
{
    std::shared_ptr<const geo::Mesh> mesh;   // always compact
    std::shared_ptr<const geo::Mesh> points; // point cloud: a mesh with only loose points
    std::shared_ptr<const CurveSet> curves;
    std::shared_ptr<const Instances> instances;

    bool empty() const;
};

LILA_SCENE_API GeometryPtr makeGeometry(Geometry g);
LILA_SCENE_API GeometryPtr meshGeometry(geo::Mesh m);
LILA_SCENE_API GeometryPtr pointGeometry(geo::Mesh m);
LILA_SCENE_API GeometryPtr curveGeometry(CurveSet c);

/** Every component transformed. Instances transform their own matrices only. */
LILA_SCENE_API Geometry transformed(const Geometry& g, const glm::dmat4& xf);
LILA_SCENE_API void transformMesh(geo::Mesh& m, const glm::dmat4& xf);
LILA_SCENE_API void transformCurves(CurveSet& c, const glm::dmat4& xf);

/** Component-wise union. Meshes are appended, instances concatenated. */
LILA_SCENE_API Geometry join(std::span<const GeometryPtr> parts);

/** Instances replaced by the geometry they point at, recursively. */
LILA_SCENE_API Geometry realize(const Geometry& g);

/** The result handed back to the modifier stack: realized, and only the mesh. Anonymous attributes are dropped. */
LILA_SCENE_API geo::Mesh toOutputMesh(const Geometry& g);

/** A point cloud has positions and point attributes; this keeps those and drops everything else. */
LILA_SCENE_API geo::Mesh pointsOnly(const geo::Mesh& m);

/** Removes "__"-prefixed attributes made for fields inside a group. */
LILA_SCENE_API void stripAnonymous(geo::Mesh& m);
} // namespace lila::gn
