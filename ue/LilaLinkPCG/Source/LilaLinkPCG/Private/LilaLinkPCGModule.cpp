#include "LilaLinkPCG.h"

#include "LilaLinkMethodRegistry.h"

#include "Modules/ModuleManager.h"

class FLilaLinkPCGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		LilaLinkPCG::RegisterMethods();
	}

	virtual void ShutdownModule() override
	{
		LilaLink::UnregisterFeature(LilaLinkPCG::Feature);
	}
};

IMPLEMENT_MODULE(FLilaLinkPCGModule, LilaLinkPCG)
