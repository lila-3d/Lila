#pragma once

#include "core/Export.h"

namespace lila::vcs
{
/** scStatus, scCheckout, scAdd, scRevert, scDelete, scSubmit, scSync, scChangelists, scHistory, scLock, scProvider. */
LILA_SCENE_API void registerCommands();
} // namespace lila::vcs
