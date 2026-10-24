#pragma once

#include "CoreMinimal.h"

class FSlateStyleSet;

/** Slate brushes for the Lila Link editor UI, from the plugin's Resources folder. */
class FLilaLinkStyle
{
public:
	static void Register();
	static void Unregister();

	static FName GetStyleSetName();

	/** The Lila icon, sized for a toolbar entry. */
	static FName GetMenuIconName() { return TEXT("LilaLink.MenuIcon"); }

private:
	static TSharedPtr<FSlateStyleSet> StyleSet;
};
