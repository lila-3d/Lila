#pragma once

#include "Seams.h"

#include <map>
#include <string>
#include <vector>

namespace seamwork
{
enum class SkirtMethod : uint8_t
{
    Auto,
    Union,
    Sweep,
};
const std::vector<std::string>& skirtMethodNames();

/** Contact skirts: a filleted band bridging each object and the ground. Lengths are cm. */
struct SkirtOptions
{
    /** How far the skirt reaches up the wall and out over the ground from the contact. */
    double width = 40.0;
    /** Fillet radius; 0 makes it follow width (each point's from its wedge angle). */
    double radius = 0.0;
    int segments = 6;
    /** Rows along the contact every this many cm (sweep). */
    double spacing = 15.0;
    SkirtMethod method = SkirtMethod::Auto;
    /** Auto sweeps a contact that turns no more than this many degrees between rows, and unions the rest. */
    double straightAngle = 25.0;
    /** Kinds taken, bit per Kind (ground joins by default). */
    int kinds = 1;
    /** Only this object's contacts; -1 for every object. */
    int object = -1;
    /** Terrain layer colours (linear RGB) by layer name, baked into vertex RGB on the ground side; grey without. */
    std::map<std::string, glm::dvec3> layerColors;
    glm::dvec3 up{0.0, 1.0, 0.0};
};

struct SkirtCurve
{
    size_t curve = 0;
    std::string method;
    /** Why that method: auto's reason, or why union fell back. */
    std::string reason;
};

struct SkirtResult
{
    Mesh mesh;
    std::vector<SkirtCurve> curves;
    std::vector<std::string> warnings;
};

/**
 * The skirt for the joins, one mesh. Its outer borders lie on the source surfaces with their normals (corner "normal"),
 * point attr "side" runs 0 at the wall border to 1 at the ground border, vertex colour RGB is the baked ground colour and
 * A the side, UV0 U along the contact and V the side.
 */
SkirtResult buildSkirt(const Result& r, std::span<const Object> objects, const Ground* ground, const SkirtOptions& o);

/** Largest turn between consecutive segments of a curve, degrees. */
double maxTurn(const Curve& c);
} // namespace seamwork
