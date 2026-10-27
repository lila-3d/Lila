#pragma once

#include "core/Export.h"

#include <cstdint>
#include <functional>

namespace lila::scene
{
class Graph;
}

/**
 * What the scene holds that viewport menu items depend on. A feature option that does nothing without some kind of node
 * (X-Ray Joints without joints, Use Default Material without materials) hides from the panel toolbar while
 * the scene has none; object-type show/hide toggles never depend on it, so hidden and absent stay distinguishable.
 */
namespace lila::ui::viewportContext
{
enum Feature : uint32_t
{
    Joints = 1u << 0,
    Materials = 1u << 1,
    /** A material whose shading model is not Default Lit (Painterly, Toon, Foliage). */
    StylisedMaterials = 1u << 2,
};

/** The features g holds, by one pass over its nodes. */
LILA_UI_API uint32_t scan(const scene::Graph& g);

/** scan() of the current graph, rescanned only after nodes are created or deleted, a scene loads or a material changes. */
LILA_UI_API uint32_t present();

/** fn runs on the main thread, coalesced, after present() changes. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);
} // namespace lila::ui::viewportContext
