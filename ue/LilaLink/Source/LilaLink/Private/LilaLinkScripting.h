#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"

#include "LilaLinkScripting.generated.h"

/** The link's JSON-RPC methods for Unreal Python and Blueprints, without a socket: what Lila sends, run in process. */
UCLASS()
class ULilaLinkScripting : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Runs one method (importAsset, setNiagaraMeshes, deleteAssets, listAssets, ...) with a JSON params object and returns
	 * the JSON-RPC response line: {"result": ...} or {"error": {...}}. Asynchronous methods are pumped (core ticker and
	 * game-thread tasks) until they answer or TimeoutSeconds pass, so this also works inside a commandlet.
	 */
	UFUNCTION(BlueprintCallable, Category = "Lila Link")
	static FString CallMethod(const FString& Method, const FString& ParamsJson, float TimeoutSeconds = 600.0f);
};
