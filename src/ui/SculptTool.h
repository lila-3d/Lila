#pragma once

#include "core/Export.h"

#include "brush/Brush.h"
#include "sculpt/Sculpt.h"

#include <functional>
#include <string>

namespace lila::ui
{
/** Brush and session options shared by the sculpt tool and panel, kept in the "general" prefs under /sculpt. */
struct SculptSettings
{
    sculpt::Brush brush = sculpt::Brush::Draw;
    /** Radius (screen px), strength, spacing, falloff (a sculpt falloff name), pressure and the rest of the shared brush. */
    brush::BrushSettings stroke = [] {
        brush::BrushSettings b;
        b.radius = 60.0;
        b.strength = 0.5;
        b.spacing = 0.1;
        b.falloff = "smooth";
        return b;
    }();
    bool frontFacesOnly = false;
    bool showCage = true;
    std::string proxyMode = "subdivide";
    int levels = 2;
    int resolution = 192;
    std::string exitMethod = "fit";
    bool keepProxy = false;
};

LILA_UI_API SculptSettings& sculptSettings();
/** Saves the settings and tells listeners (the panel, the viewports). */
LILA_UI_API void sculptSettingsChanged();
LILA_UI_API int addSculptSettingsListener(std::function<void()> fn);
LILA_UI_API void removeSculptSettingsListener(int id);

/**
 * The sculpt viewport tool, on the open sculpt session:
 *
 *   LMB drag             stroke with the current brush
 *   Ctrl                 inverts (dig, unmask, unfreeze, magnify)
 *   Shift                smooths for the stroke
 *   F or B held + drag   brush radius (F tapped still frames the selection)
 *   M or Shift+F + drag  brush strength
 *   [ ]                  radius smaller / larger
 *   Esc                  leaves the tool; the session stays open until Exit Sculpt
 *
 * Each stroke runs one sculptStroke command, so it is one undo step.
 */
LILA_UI_API void startSculptTool();
LILA_UI_API bool sculptToolActive();

/** Draws the open session's proxy in place of its mesh in every viewport, with the cage wire and the mask. */
LILA_UI_API void installSculptDrawer();
} // namespace lila::ui
