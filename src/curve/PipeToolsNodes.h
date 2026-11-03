#pragma once

/**
 * Pipe and channel tools in the scene: the "curvePipeRoute" node and pipeRoute command (an orthogonal run around
 * obstacle meshes), the "curvePipeSupports" node and pipeSupports command (hangers, brackets and stands reaching to
 * surfaces), and the "channelModifier" with the carveChannel command (a profile swept along a curve and cut into a mesh).
 */
namespace lila::curve
{
void registerPipeToolTypes();
void registerPipeToolCommands();
} // namespace lila::curve
