#pragma once

#include "core/Export.h"

#include "brush/Brush.h"
#include "sculpt/Sculpt.h"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lila::ui
{
/** Brush and session options shared by the sculpt tool and panel, kept in the "general" prefs under /sculpt. */
struct SculptSettings
{
    sculpt::Brush brush = sculpt::Brush::Draw;
    /** The registered brush's name when brush is Custom. */
    std::string custom;
    /** Shape brushes paint polypaint too; the Paint brush always does. */
    bool paint = false;
    /** Polypaint color, linear RGB. */
    glm::vec3 color{1.0f};
    /** The current brush's radius (screen px), strength, spacing, falloff, pressure and the rest of the shared brush core. */
    brush::BrushSettings stroke = [] {
        brush::BrushSettings b;
        b.radius = 60.0;
        b.strength = 0.5;
        b.spacing = 0.1;
        b.falloff = "smooth";
        return b;
    }();
    /**
     * Each brush keeps its own stroke settings, swapped in when it is picked, except what is shared: the radius, the
     * strength, and the rest of the stroke (falloff, spacing, alpha, stabiliser...) each follow from brush to brush when shared.
     */
    std::map<std::string, brush::BrushSettings> perBrush;
    bool shareRadius = true;
    bool shareStrength = false;
    bool shareStroke = false;
    bool frontFacesOnly = false;
    bool showCage = true;
    /** The sculpt mesh's wireframe (not the cage's). */
    bool wireframe = false;
    /** A sculptMaterials() id. */
    std::string material = "clay";
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

/** Picks a brush (custom names a registered one): stores the old brush's stroke settings and swaps in the new one's. */
LILA_UI_API void selectSculptBrush(sculpt::Brush brush, const std::string& custom = {});
/** The preview materials sculpt mode can draw with, as (id, label). */
LILA_UI_API std::vector<std::pair<std::string, std::string>> sculptMaterials();

/**
 * The sculpt viewport tool, on the open sculpt session:
 *
 *   LMB drag             stroke with the current brush
 *   Ctrl                 inverts (dig, unmask, unfreeze, magnify)
 *   Shift                smooths for the stroke
 *   F or B held + drag   brush radius (F tapped still frames the selection)
 *   M or Shift+F + drag  brush strength (or B held + RMB drag)
 *   Shift+F tapped       the sculpt mesh's wireframe on or off
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
