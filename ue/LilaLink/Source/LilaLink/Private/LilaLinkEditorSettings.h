#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "LilaLinkEditorSettings.generated.h"

/** Editor Preferences > Plugins > Lila Link Editor. Per developer, never checked in. */
UCLASS(Config = EditorPerProjectUserSettings, meta = (DisplayName = "Lila Link Editor"))
class ULilaLinkEditorUserSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Show the Lila dropdown in the level editor toolbar. */
	UPROPERTY(Config, EditAnywhere, Category = "Toolbar")
	bool bShowToolbarMenu = true;

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
};
