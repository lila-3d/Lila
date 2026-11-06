#include "LilaLinkMeshTerrain.h"

#include "LilaLinkMethodRegistry.h"

#include "Modules/ModuleManager.h"

class FLilaLinkMeshTerrainModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		LilaLinkMeshTerrain::RegisterMethods();
	}

	virtual void ShutdownModule() override
	{
		LilaLink::UnregisterFeature(LilaLinkMeshTerrain::Feature);
	}
};

IMPLEMENT_MODULE(FLilaLinkMeshTerrainModule, LilaLinkMeshTerrain)
