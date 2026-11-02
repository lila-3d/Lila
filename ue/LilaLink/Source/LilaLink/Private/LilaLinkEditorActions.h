#pragma once

#include "CoreMinimal.h"

/** What the Lila toolbar dropdown does to the Content Browser selection. Each reports through a toast and LogLilaLink. */
namespace LilaLinkActions
{
/** Reruns the import of each selected asset whose source file is in Lila's export cache, with its stored settings. */
void ReimportSelected();
bool CanReimportSelected();

/** Opens the file browser on the first selected asset's source file. */
void ShowSourceFile();
bool CanShowSourceFile();

/** The VAT setup on each selected static mesh, from the bake .json beside its source file or one picked by hand. */
void SetUpSelectedMeshesForVat();
bool HasSelectedMeshes();

/** VAT data texture settings on the selected textures; names with nrm or normal in them are the normal texture. */
void SetUpSelectedTexturesAsVatData();
bool HasSelectedTextures();

/** The selected static meshes, in name order, into every mesh renderer of a Niagara system. */
void FillNiagaraFromSelectedMeshes();

/** Address Clamp on the selected textures, and the normal map texture group on normal maps. */
void SetUpSelectedTexturesForUniqueUVs();

/** Address Wrap on the selected textures, and the normal map texture group on normal maps. */
void SetUpSelectedTexturesForTiling();

/** Builds M_LilaTrim, the trim sheet master, on MF_LilaTrim in the masters folder; rebuilds both in place when they already exist. */
void GenerateTrimMaster();
} // namespace LilaLinkActions
