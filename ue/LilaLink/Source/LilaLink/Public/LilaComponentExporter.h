#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;

/**
 * Applies a Lila attribute component whose Unreal export is "custom" with this exporter's name. Register one from your
 * editor module (depend on LilaLink and Json) with LilaLink::RegisterComponentExporter in StartupModule and unregister it
 * in ShutdownModule. LilaLinkExampleExporter.cpp is a small example.
 */
class ILilaComponentExporter
{
public:
	virtual ~ILilaComponentExporter() = default;

	/** What a component's Unreal export names as its class in Lila. */
	virtual FName GetName() const = 0;

	/**
	 * Called on the game thread inside the send's transaction. Actor is the placed actor (unrealPlaceInstances); it is null
	 * when the component arrives with an asset import, and Asset is that mesh, animation or other imported asset. Attrs
	 * holds the attribute values by name as Lila sends them: numbers, bools, strings, enum labels, [x, y, z] arrays.
	 * Warnings go back to Lila. Return false when the component could not be applied.
	 */
	virtual bool Apply(AActor* Actor, UObject* Asset, const FString& ComponentName, const TSharedRef<FJsonObject>& Attrs, TArray<FString>& OutWarnings) = 0;
};

namespace LilaLink
{
/** Replaces any exporter of the same name. */
LILALINK_API void RegisterComponentExporter(const TSharedRef<ILilaComponentExporter>& Exporter);
LILALINK_API void UnregisterComponentExporter(FName Name);
/** The C++ exporters' names, then those registered in Python through lila_link_components. */
LILALINK_API TArray<FString> ComponentExporterNames();
} // namespace LilaLink
