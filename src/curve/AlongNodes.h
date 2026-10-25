#pragma once

/**
 * The along-curve modifiers ("curveDeformModifier", "curveArrayModifier", "surfaceFlowModifier"), their commands, and
 * the curve deformer commands (bendCurve, curlCurve, scaleCurvature, smoothCurve).
 */
namespace lila::curve
{
void registerAlongTypes();
void registerAlongCommands();
/** CurveDeformCommands.cpp. */
void registerCurveDeformCommands();
} // namespace lila::curve
