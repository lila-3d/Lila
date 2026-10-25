#pragma once

#include "core/Export.h"

#include "scene/Skin.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace lila::scene::skin
{
/** Skins with the node's centres of rotation or direct delta mush method. The palette is indexed by influence id. */
MeshPtr skinWithMethod(const SkinClusterNode& node, const geo::Mesh& in, const SkinWeights& w, const std::vector<glm::dmat4>& palette, double envelope);

/** "linear", "dualQuaternion", "centresOfRotation" or "directDeltaMush" to SkinClusterNode::Method; throws CommandError. */
LILA_SCENE_API int64_t methodOf(const std::string& name, const char* cmd);
} // namespace lila::scene::skin
