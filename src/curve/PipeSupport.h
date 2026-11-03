#pragma once

#include "core/Export.h"
#include "curve/Cable.h"
#include "curve/Pipe.h"

#include <functional>
#include <optional>

/** Pipe supports: hangers, wall brackets, floor stands and clamps placed along a run, reaching to the nearest surface. */
namespace lila::curve
{
enum class SupportStyle : uint8_t
{
    Auto,    // hanger to a ceiling, bracket to a wall, stand to a floor
    Hanger,  // a clamp ring on a rod with a ceiling plate
    Bracket, // a clamp ring on an arm with a brace and a wall plate
    Stand,   // a cradle on a post with a base plate
    Clamp,   // a clamp ring on a strut with a plate, any direction
    Mesh,    // the given mesh
};
LILA_SCENE_API const char* supportStyleName(SupportStyle s);

enum class SupportTarget : uint8_t
{
    Nearest, // the closest of floor, ceiling and the walls either side
    Floor,
    Ceiling,
    Wall,
};
LILA_SCENE_API const char* supportTargetName(SupportTarget t);

/** The nearest surface along a ray from origin (dir unit length) within maxDistance cm. */
using SurfaceRay = std::function<std::optional<SurfacePoint>(const glm::dvec3& origin, const glm::dvec3& dir, double maxDistance)>;

struct SupportOptions
{
    SupportStyle style = SupportStyle::Auto;
    SupportTarget target = SupportTarget::Nearest;
    /** Supports every this many cm along the run, the first at startOffset (negative: half the spacing). */
    double spacing = 200.0;
    double startOffset = -1.0;
    /** Keep supports this far (cm) from the run's ends and off its bends (0: twice the pipe radius). */
    double endClearance = 0.0;
    /** The pipe they hold, and the bend radius it rounds corners with (0: twice the radius), so supports sit on it. */
    double pipeRadius = 5.0;
    double bendRadius = 0.0;
    /** Farthest a support reaches, cm; stations with no surface that close get none. */
    double maxReach = 500.0;
    /** Clamp ring thickness and width (0: from the pipe radius), rod or arm thickness (0: from the radius). */
    double ringThickness = 0.0;
    double ringWidth = 0.0;
    double rodSize = 0.0;
    int sides = 12;
    glm::dvec3 up{0.0, 1.0, 0.0};
    /** Style Mesh: placed with its +y toward the surface and +z along the pipe; stretched along y to reach it when stretch. */
    const geo::Mesh* mesh = nullptr;
    double meshScale = 1.0;
    bool stretch = true;
    SurfaceRay cast;
};

struct SupportPlacement
{
    /** Arc length along the run, the pipe centre, the surface point and its normal, and the direction reached in. */
    double s = 0.0;
    glm::dvec3 at{0.0};
    glm::dvec3 hit{0.0};
    glm::dvec3 normal{0.0};
    glm::dvec3 dir{0.0};
    double reach = 0.0;
    SupportStyle style = SupportStyle::Hanger;
};

struct SupportResult
{
    /** Faces carry "pipePart" = Support and "part" = the support's index. */
    geo::Mesh mesh;
    std::vector<SupportPlacement> placements;
    std::vector<std::string> warnings;
};

/** Supports along the path (world space in and out), each reaching to the surface `cast` finds. */
LILA_SCENE_API SupportResult makeSupports(const Spline& path, const SupportOptions& o);
} // namespace lila::curve
