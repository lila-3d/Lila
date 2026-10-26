#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Materials for Lila's masonry walls: one master, M_LilaMasonry in the masters folder, built the first time it is asked
 * for and then left to artists (regenerate rebuilds its graph in place, keeping references), and an instance per wall
 * slot or shell.
 *
 * masonryMaterials {folder, regenerate?, parent?, wallHeight, meshes: [static mesh paths], instances: [{name, slot,
 *   meshes?: [...], scalars: {param: value}, vectors: {param: [r, g, b]}, textures: {param: texture asset path},
 *   files: {param: {file, normal?, flipGreen?}}}]}
 * makes or updates each instance in folder (parent, when given, replaces the master), imports files as textures next to
 * it, and sets it on every slot named slot of the instance's meshes (all meshes when it lists none). Returns {master,
 * masterVersion, built, instances: [...], assigned: [{mesh, slot, material}], created, warnings}.
 */
void AddMasonryMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
