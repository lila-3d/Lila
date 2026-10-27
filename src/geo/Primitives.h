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

/** How the flat end of a cylinder or cone is filled inside its rings. */
enum class CapFill : uint8_t
{
    /** A grid of quads joined to the rim by a ring of quads; one triangle when the side count is odd. Wedges pair up their fan. */
    Quads,
    /** Triangles meeting at the centre. */
    Fan,
    /** One polygon. */
    Ngon,
    /** Left open. */
    None,
};

struct SphereOptions
{
    double radius = 50.0;
    int subdivAxis = 20;
    int subdivHeight = 20;
    bool baseAtOrigin = false;
    /** Portion of the height kept, from the top: 1 is whole, 0.5 a dome open at the bottom (-Y). */
    double fraction = 1.0;
};

struct CylinderOptions
{
    double radius = 50.0;
    double height = 100.0;
    int subdivAxis = 20;
    int subdivHeight = 1;
    /** Rings of quads around each cap's fill; 0 and 1 both leave just the fill. */
    int subdivCaps = 0;
    bool caps = true;
    bool baseAtOrigin = false;
    /** Portion of a full turn swept around Y from +X: 1 is whole, 0.5 half. */
    double fraction = 1.0;
    /** Close the two flat ends a partial sweep leaves. */
    bool cutCaps = true;
    CapFill capFill = CapFill::Quads;
};

struct CapsuleOptions
{
    double radius = 50.0;
    /** End to end, caps included; never less than the diameter. */
    double height = 200.0;
    int subdivAxis = 20;
    int subdivHeight = 1;
    /** Rings on each hemispherical cap. */
    int subdivCaps = 6;
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
    /** Portion of a full turn swept around Y from +X: 1 is whole, 0.5 half. */
    double fraction = 1.0;
    /** Close the two flat ends a partial sweep leaves. */
    bool cutCaps = true;
    CapFill capFill = CapFill::Quads;
    /** Cut the point off at tipRadius and fill it like the base, so a quads fill leaves no triangles at the tip. */
    bool capTip = false;
    double tipRadius = 0.05;
};

struct TorusOptions
{
    double radius = 50.0;
    double sectionRadius = 20.0;
    int subdivAxis = 20;
    int subdivHeight = 20;
    /** Portion of a full turn swept around Y from +X: 1 is whole, 0.5 half. */
    double fraction = 1.0;
    /** Close the two flat ends a partial sweep leaves. */
    bool cutCaps = true;
};

struct DiscOptions
{
    double radius = 50.0;
    int subdivAxis = 20;
    /** 0 makes a single n-gon. */
    int subdivRings = 1;
    /** Portion of a full turn swept around Y from +X: 1 is whole, 0.5 half. */
    double fraction = 1.0;
    /** How the innermost ring is filled; None counts as Fan, since a disc is all fill. */
    CapFill capFill = CapFill::Fan;
};

struct PipeOptions
{
    double radius = 50.0;
    double thickness = 10.0;
    double height = 100.0;
    int subdivAxis = 20;
    int subdivHeight = 1;
    bool baseAtOrigin = false;
    /** Portion of a full turn swept around Y from +X: 1 is whole, 0.5 half. */
    double fraction = 1.0;
    /** Close the two flat ends a partial sweep leaves. */
    bool cutCaps = true;
};

struct QuadSphereOptions
{
    double radius = 50.0;
    int subdiv = 3;
    /** Portion of the height kept, from the top: 1 is whole, 0.5 a dome open at the bottom (-Y). */
    double fraction = 1.0;
};

struct IcoSphereOptions
{
    double radius = 50.0;
    /** 1 is the plain icosahedron; each level splits every triangle in four. */
    int subdivisions = 3;
    bool baseAtOrigin = false;
    /** Portion of the height kept, from the top: 1 is whole, 0.5 a dome open at the bottom (-Y). */
    double fraction = 1.0;
};

LILA_SCENE_API Mesh makeCube(const CubeOptions& o = {});
/** In XZ facing +Y. */
LILA_SCENE_API Mesh makePlane(const PlaneOptions& o = {});
LILA_SCENE_API Mesh makeSphere(const SphereOptions& o = {});
LILA_SCENE_API Mesh makeCylinder(const CylinderOptions& o = {});
LILA_SCENE_API Mesh makeCapsule(const CapsuleOptions& o = {});
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
