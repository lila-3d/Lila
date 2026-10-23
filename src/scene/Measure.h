#pragma once

#include "core/Export.h"

#include "scene/Transform.h"

namespace lila::scene
{
/**
 * Live measurement nodes, Maya's distanceDimension family. Each is a transform (so it lists, hides and saves like any
 * object) whose own matrix is ignored: the measured points arrive as world matrices connected from locators or other
 * objects, or for radiusMeasure from a mesh, so parenting or constraining an endpoint keeps the value live.
 */
class MeasureNode : public Transform
{
};

class LILA_SCENE_API DistanceMeasure : public MeasureNode
{
public:
    enum Attr
    {
        kStartMatrix = Transform::kCount,
        kEndMatrix,
        kStartPoint,
        kEndPoint,
        kDistance,
        kDelta,
        kCount
    };
    void compute(const EvalContext& ctx) override;
};

class LILA_SCENE_API AngleMeasure : public MeasureNode
{
public:
    enum Attr
    {
        kPoint1Matrix = Transform::kCount,
        kVertexMatrix,
        kPoint2Matrix,
        kAngle,
        kCount
    };
    void compute(const EvalContext& ctx) override;
};

/**
 * Best-fit circle through a mesh's vertices (inMesh + meshMatrix + vertices, live under mesh edits) or through fixed
 * world points (points, a JSON list of [x, y, z]) when no mesh is connected.
 */
class LILA_SCENE_API RadiusMeasure : public MeasureNode
{
public:
    enum Attr
    {
        kInMesh = Transform::kCount,
        kMeshMatrix,
        kVertices,
        kPoints,
        kDisplay,
        kValid,
        kRadius,
        kDiameter,
        kCircumference,
        kCenter,
        kNormal,
        kPlanarity,
        kFitError,
        kCount
    };
    enum Display : int64_t
    {
        kShowRadius,
        kShowDiameter,
        kShowBoth,
    };
    void compute(const EvalContext& ctx) override;
};

namespace measure
{
LILA_SCENE_API bool isMeasure(const Node& n);
/** Measurements are drawn unless the general pref /measure/show is off. */
LILA_SCENE_API bool shown();
/** Value label for a measure node in display units, e.g. "12.50 cm" or "R 5.00 cm  Ø 10.00 cm". */
LILA_SCENE_API std::string label(const Node& n);
} // namespace measure

/** distanceMeasure, angleMeasure, radiusMeasure types. Idempotent; needs the transform type first. */
LILA_SCENE_API void registerMeasureTypes();
/** distanceMeasure, angleMeasure, radiusMeasure and measureSelection commands. Idempotent. */
LILA_SCENE_API void registerMeasureCommands();
} // namespace lila::scene
