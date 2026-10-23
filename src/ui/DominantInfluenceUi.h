#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * The "lila.dominantInfluence" selection handler (a click on a skinned mesh selects the joint that dominates the face,
 * hovering washes that joint's weights over the mesh), its toggle action "rig.dominantInfluenceSelect" (pref
 * general /rig/dominantInfluenceSelect), and the centre-in-mesh actions. Idempotent.
 */
LILA_UI_API void registerDominantInfluenceUi();
} // namespace lila::ui
