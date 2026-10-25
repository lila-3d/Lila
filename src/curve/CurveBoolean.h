#pragma once

#include "core/Export.h"
#include "curve/Ops.h"
#include "curve/Planar.h"

#include <string>

/** 2D booleans and region offsets of closed planar curves (Clipper2 on tessellations, exact curve pieces back out). */
namespace lila::curve
{
enum class BooleanOp : uint8_t
{
    Union,
    Subtract,
    Intersect,
    Xor,
};
LILA_SCENE_API const char* booleanOpName(BooleanOp op);
LILA_SCENE_API bool parseBooleanOp(std::string_view name, BooleanOp* out);

struct CurveBooleanOptions
{
    BooleanOp op = BooleanOp::Union;
    /** Plane normal; zero fits the plane to the curves. */
    glm::dvec3 normal{0.0};
    /** Tessellation chord error for the clip, cm. Results are cut from the original curves, so this only places the cuts. */
    double tolerance = 0.002;
    /** Curves further than this off their common plane (cm) give a warning. */
    double planarTolerance = 0.01;
    /** Operands fill non-zero (by loop direction, as fonts do) instead of even-odd. */
    bool nonZero = false;
};

struct CurveBooleanResult
{
    /** Outer loops counter-clockwise about the plane normal, holes clockwise. */
    Splines curves;
    PlaneFrame plane;
    std::vector<std::string> warnings;
};

/**
 * Combines regions: each operand is a set of closed curves filled even-odd (a letter with its counter is one operand).
 * The first operand is the target; union joins them all, subtract removes every other operand from the target, intersect
 * keeps what all share and xor keeps what an odd number cover. Every piece of the result is cut exactly from the input
 * curve it lies on (Bezier stays Bezier, circles stay exact), with sharp corners where boundaries cross.
 */
LILA_SCENE_API CurveBooleanResult curveBoolean(const std::vector<Splines>& operands, const CurveBooleanOptions& o);

struct RegionOffsetOptions
{
    /** cm; positive grows the region, negative shrinks it (an inset). */
    double distance = 1.0;
    JoinType join = JoinType::Round;
    double miterLimit = 4.0;
    glm::dvec3 normal{0.0};
    /** Fitting tolerance for curved results, cm. */
    double tolerance = 0.01;
};

/**
 * Offsets the region closed curves enclose (even-odd, so holes shrink as the outline grows), merging loops that run into
 * each other and dropping parts that vanish. Straight runs stay straight with crisp corners; curved runs are refit as
 * Beziers (or poly when every input is poly).
 */
LILA_SCENE_API CurveBooleanResult regionOffset(const Splines& curves, const RegionOffsetOptions& o);

/** The part of s from span parameter u0 running delta spans on (negative runs backwards), as Bezier pieces of s. */
LILA_SCENE_API std::vector<Piece> subPieces(const Spline& s, double u0, double delta);
} // namespace lila::curve
