#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "LilaLinkSettings.generated.h"

/** Editor Preferences > Plugins > Lila Link. Per user, per project. */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Lila Link"))
class ULilaLinkSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Run the localhost server Lila connects to. */
	UPROPERTY(config, EditAnywhere, Category = "Server")
	bool bEnabled = true;

	/** First port to try; the next few are tried if it is taken. */
	UPROPERTY(config, EditAnywhere, Category = "Server", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 Port = 8741;

	/** Save packages touched by an import as soon as it finishes. */
	UPROPERTY(config, EditAnywhere, Category = "Import")
	bool bAutoSaveAfterImport = false;

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
};
