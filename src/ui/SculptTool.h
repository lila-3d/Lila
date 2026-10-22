#pragma once

#include "sculpt/Sculpt.h"

#include <functional>
#include <string>

namespace lila::ui
{
/** Brush and session options shared by the sculpt tool and panel, kept in the "general" prefs under /sculpt. */
struct SculptSettings
{
    sculpt::Brush brush = sculpt::Brush::Draw;
    sculpt::Falloff falloff = sculpt::Falloff::Smooth;
    double radius = 60.0; // screen px
    double strength = 0.5;
    /** Dab spacing as a fraction of the radius. */
    double spacing = 0.1;
    bool frontFacesOnly = false;
    bool showCage = true;
    std::string proxyMode = "subdivide";
    int levels = 2;
    int resolution = 192;
    std::string exitMethod = "fit";
    bool keepProxy = false;
};

SculptSettings& sculptSettings();
/** Saves the settings and tells listeners (the panel, the viewports). */
void sculptSettingsChanged();
int addSculptSettingsListener(std::function<void()> fn);
void removeSculptSettingsListener(int id);

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
void startSculptTool();
bool sculptToolActive();

/** Draws the open session's proxy in place of its mesh in every viewport, with the cage wire and the mask. */
void installSculptDrawer();
} // namespace lila::ui
