#pragma once

#include "core/Export.h"

#include "render/RenderMesh.h"

#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::ui
{
/** The viewport's textured toggle (general pref /viewport/materials). Off draws every mesh flat gray, for form checks. */
LILA_UI_API bool materialsShown();
LILA_UI_API void setMaterialsShown(bool on);

/** True when a slot of the mesh binds to a material with a normal map, so its render copy needs tangents. */
LILA_UI_API bool needsTangents(const geo::Mesh& mesh);

/**
 * Renderer materials for every slot of the mesh, in slot order, from the material nodes of the current graph that
 * share the slot names. Slots without a node get the neutral default. Maps still loading are left out until they arrive.
 */
LILA_UI_API std::vector<render::Material> drawMaterials(const geo::Mesh& mesh);
} // namespace lila::ui
