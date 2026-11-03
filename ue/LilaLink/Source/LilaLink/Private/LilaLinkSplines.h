#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's curve to spline sends:
 *
 * setSpline {id, label, mode, location, splines, mesh?, landscape?, width?, deformTerrain?, join?, folder?} builds what Lila's curve id
 * makes, or updates it in place when it already exists. mode "actor" is an actor at location (Unreal space, cm) with a spline
 * component per entry of splines and, with mesh {asset, forwardAxis}, a spline mesh component per segment. mode "landscape"
 * adds control points and segments to the landscape's splines (a landscape spline actor in a partitioned world), with mesh as
 * their spline mesh; with join, splines meeting at one point share its control point. Each spline is {closed, points: [{position (relative to location), arriveTangent, leaveTangent, up, roll,
 * scale}]}. Replies {actor, components, segments, warnings}.
 *
 * removeSpline {id} deletes what setSpline made for id. Replies {removed}.
 */
void AddSplineMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
