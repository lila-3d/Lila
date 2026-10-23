#pragma once

#include <lila/Extension.h>

/** Node types and attribute names shared by the scene and UI halves of the Overlap extension. */
namespace overlap
{
inline constexpr const char* kOverlapType = "overlap";
inline constexpr const char* kChainType = "overlapChain";
inline constexpr const char* kJointType = "overlapJoint";
inline constexpr const char* kColliderType = "overlapCollider";

/** Node types, the solver type and the commands. Returns false when a type could not be registered. */
bool registerScene(lila::ext::Context& ctx);
/** Viewport drawing, actions, menus, the panel and Attribute Editor sections. */
void registerUi(lila::ext::Context& ctx);
} // namespace overlap
