#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

namespace lila::geo
{
// All primitives are Y-up, in centimetres, centred on the origin unless baseAtOrigin is set, with
// non-overlapping "uv0", hard edges where the surface creases, and "seam" on UV island borders.

struct CubeOptions
{
    double width = 100.0;
    double height = 100.0;
    double depth = 100.0;
    int subdivX = 1;
    int subdivY = 1;
    int subdivZ = 1;
    bool baseAtOrigin = false;
};

struct PlaneOptions
{
    double width = 100.0;
    double depth = 100.0;
    int subdivX = 1;
    int subdivZ = 1;
};

struct SphereOptions
{
    double radius = 50.0;
    int subdivAxis = 20;
    int subdivHeight = 20;
    bool baseAtOrigin = false;
};

struct CylinderOptions
{
    double radius = 50.0;
    double height = 100.0;
    int subdivAxis = 20;
    int subdivHeight = 1;
    /** 0 makes each cap a single n-gon. */
    int subdivCaps = 0;
    bool caps = true;
    bool baseAtOrigin = false;
};

struct ConeOptions
{
    double radius = 50.0;
    double height = 100.0;
    int subdivAxis = 20;
    int subdivHeight = 1;
    int subdivCap = 0;
    bool baseAtOrigin = false;
};

struct TorusOptions
{
    double radius = 50.0;
    double sectionRadius = 20.0;
    int subdivAxis = 20;
    int subdivHeight = 20;
};

struct DiscOptions
{
    double radius = 50.0;
    int subdivAxis = 20;
    /** 0 makes a single n-gon. */
    int subdivRings = 1;
};

struct PipeOptions
{
    double radius = 50.0;
    double thickness = 10.0;
    double height = 100.0;
    int subdivAxis = 20;
    int subdivHeight = 1;
    bool baseAtOrigin = false;
};

struct QuadSphereOptions
{
    double radius = 50.0;
    int subdiv = 8;
};

struct IcoSphereOptions
{
    double radius = 50.0;
    /** 1 is the plain icosahedron; each level splits every triangle in four. */
    int subdivisions = 3;
    bool baseAtOrigin = false;
};

LILA_SCENE_API Mesh makeCube(const CubeOptions& o = {});
/** In XZ facing +Y. */
LILA_SCENE_API Mesh makePlane(const PlaneOptions& o = {});
LILA_SCENE_API Mesh makeSphere(const SphereOptions& o = {});
LILA_SCENE_API Mesh makeCylinder(const CylinderOptions& o = {});
LILA_SCENE_API Mesh makeCone(const ConeOptions& o = {});
LILA_SCENE_API Mesh makeTorus(const TorusOptions& o = {});
/** In XZ facing +Y. */
LILA_SCENE_API Mesh makeDisc(const DiscOptions& o = {});
LILA_SCENE_API Mesh makePipe(const PipeOptions& o = {});
/** Subdivided cube projected onto a sphere: all quads, no poles. */
LILA_SCENE_API Mesh makeQuadSphere(const QuadSphereOptions& o = {});
/** Geodesic sphere of near-equal triangles: no poles, no pinching. */
LILA_SCENE_API Mesh makeIcoSphere(const IcoSphereOptions& o = {});

/** Sets edge "seam" where the corners on either side disagree on the UV set. Returns the count. */
LILA_SCENE_API Index markUvSeams(Mesh& mesh, std::string_view uvSet = "uv0");
} // namespace lila::geo
