#pragma once

#include "render/RenderMesh.h"

#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::ui
{
/** The viewport's textured toggle (general pref /viewport/materials). Off draws every mesh flat gray, for form checks. */
bool materialsShown();
void setMaterialsShown(bool on);

/** True when a slot of the mesh binds to a material with a normal map, so its render copy needs tangents. */
bool needsTangents(const geo::Mesh& mesh);

/**
 * Renderer materials for every slot of the mesh, in slot order, from the material nodes of the current graph that
 * share the slot names. Slots without a node get the neutral default. Maps still loading are left out until they arrive.
 */
std::vector<render::Material> drawMaterials(const geo::Mesh& mesh);
} // namespace lila::ui
