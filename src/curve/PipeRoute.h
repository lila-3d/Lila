#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <functional>
#include <string>
#include <vector>

/** Pipe routing: an orthogonal run between two points around obstacles, found on a grid (A* over cells and headings). */
namespace lila::curve
{
struct PipeRouteOptions
{
    /** Least distance from the centreline to any obstacle, cm (the pipe's radius plus a gap). */
    double clearance = 10.0;
    /** Grid step, cm; 0 picks one from the clearance and the distance. Each axis is adjusted so both ends sit on the grid. */
    double cellSize = 0.0;
    /** Cost per cm along x, y and z: above 1 makes runs along that axis less likely (2 on y keeps risers short). */
    glm::dvec3 axisCost{1.0, 1.0, 1.0};
    /** Cost of each bend, in cm of run. */
    double bendCost = 100.0;
    /** The run leaves the start and arrives at the end along these (zero: any axis); snapped to the nearest axis. */
    glm::dvec3 startDir{0.0};
    glm::dvec3 endDir{0.0};
    /** How far the grid reaches past the box around the ends, cm (0: a few cells and the clearance). */
    double margin = 0.0;
    /** Largest grid; the step grows until the box fits. */
    size_t maxCells = 4'000'000;
    /** Cells the search may expand before it gives up. */
    size_t maxExpanded = 2'000'000;
};

struct PipeRouteResult
{
    /** Start, every corner, end. A straight line from start to end when no route was found. */
    std::vector<glm::dvec3> points;
    bool found = false;
    double length = 0.0;
    int bends = 0;
    /** The grid step used per axis, cm. */
    glm::dvec3 cell{0.0};
    std::string warning;
};

/** Signed distance from a world point to the obstacles (negative inside), up to the distance given. */
using ObstacleDistance = std::function<double(const glm::dvec3& p, double maxDistance)>;

LILA_SCENE_API PipeRouteResult routePipe(const glm::dvec3& start, const glm::dvec3& end, const ObstacleDistance& obstacles,
                                         const PipeRouteOptions& o = {});

/** The route as an open poly spline through its corners. */
LILA_SCENE_API Spline routeSpline(const PipeRouteResult& r);
} // namespace lila::curve
