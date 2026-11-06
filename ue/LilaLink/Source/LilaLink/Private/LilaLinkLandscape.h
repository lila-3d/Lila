#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's terrain sends:
 *
 * setLandscape {id, label, heightmap, resolution: [x, y], quadsPerSection, sectionsPerComponent, location, scale, material?,
 * gridSize?} makes the landscape Lila's terrain id sends, or updates it in place. heightmap is a raw 16-bit file in Unreal's
 * layout, resolution vertices a side, read on this machine. Lila's heights go to an edit layer named "Lila", so layers the
 * user adds on top survive later sends. A landscape whose resolution or layout changed is rebuilt. gridSize > 0 sets the
 * World Partition grid size in a partitioned world. Replies {actor, created, resolution, editLayer}.
 *
 * removeLandscape {id} deletes it. Replies {removed}.
 */
void AddLandscapeMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink

class ALandscape;
class UMaterial;
class ULandscapeLayerInfoObject;

/** The landscape send's building blocks, shared with the region import (LilaLinkLandscapeRegions.cpp). */
namespace LilaLink::Landscape
{
UWorld* EditorWorld();
/** The landscape Lila's terrain id sent, or null. */
ALandscape* FindLandscape(UWorld* World, const FString& Id);
FName LandscapeTag(const FString& Id);
/** Same vertex count and section layout. */
bool HasLayout(ALandscape* Landscape, int32 SizeX, int32 SizeY, int32 QuadsPerSection, int32 SectionsPerComponent);
/** A flat landscape at the encoding's zero, ready for heights on the Lila edit layer. */
ALandscape* CreateFlatLandscape(UWorld* World, const FString& Label, const FVector& Location, const FRotator& Rotation, const FVector& Scale, int32 SizeX,
	int32 SizeY, int32 QuadsPerSection, int32 SectionsPerComponent);
/** The "Lila" edit layer's guid, made when missing. */
FGuid LilaEditLayer(ALandscape* Landscape);
/** M_<Label> on MF_LilaLandscapeLayer sampling these layers. */
UMaterial* GeneratedMaterial(const FString& Label, const TArray<FName>& Layers, TArray<FString>& OutCreated, TArray<FString>& OutWarnings);
/** LI_<Layer> next to the landscape's material, made when missing. */
ULandscapeLayerInfoObject* LayerInfoFor(const FString& Label, FName Layer, TArray<FString>& OutCreated);
} // namespace LilaLink::Landscape
