#pragma once

// Anim proxies under Puppeteer: the prefs that turn them on with Puppeteer's modes, and the Spinner's toggles.

#include "Common.h"

#include <vector>

namespace puppeteer
{
/** Prefs keys in the puppeteer prefs file. */
inline constexpr const char* kUseAnimProxy = "/animProxy/use";
inline constexpr const char* kProxyOnEnter = "/animProxy/showOnEnter";
inline constexpr const char* kRealOnExit = "/animProxy/realOnExit";

/** Use anim proxy is on (the default). */
bool animProxyEnabled();
/** The proxy sets of the selected characters: of the selected joints, pieces, real meshes or sets. */
std::vector<NodeId> selectedProxySets();
/** Whether the first of those shows its proxies. */
bool selectionShowsProxy();
/** Shows the selection's characters as proxy or real (undoable), or every set when nothing selected has one. */
void toggleSelectedProxy();
/** The selected joints' true pieces, shown alone or back to following the set. */
void toggleSelectedRealPiece();
} // namespace puppeteer
