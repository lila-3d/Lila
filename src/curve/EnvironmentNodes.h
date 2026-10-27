#pragma once

/**
 * Environment curve tools in the scene: the "curveContour" history node with the contour and section commands
 * (ContourNode.cpp), the "curveCable" node with the cable command (CableNode.cpp), and conformCurve, which keeps curves on
 * a live surface (LiveSurface.cpp), and ezCurve, a curve between two points that adapts to the meshes it runs over (EzCurve.cpp).
 */
namespace lila::curve
{
void registerContourType();
void registerContourCommands();
void registerCableType();
void registerCableCommands();
void registerLiveSurfaceCommands();
void registerEzCurveCommand();
} // namespace lila::curve
