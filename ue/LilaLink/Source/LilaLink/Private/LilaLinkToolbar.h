#pragma once

#include "CoreMinimal.h"

/** What the dropdown's status line reads and how Restart Link restarts the server. The module supplies all three. */
struct FLilaLinkToolbarHooks
{
	TFunction<FText()> Status;
	/** False while the server is off in its settings, when there is nothing to restart. */
	TFunction<bool()> CanRestart;
	TFunction<void()> Restart;
};

/** The Lila dropdown on the level editor toolbar, shown while ULilaLinkEditorUserSettings::bShowToolbarMenu is set. */
namespace LilaLinkToolbar
{
void Register(FLilaLinkToolbarHooks Hooks);
void Unregister();
} // namespace LilaLinkToolbar
