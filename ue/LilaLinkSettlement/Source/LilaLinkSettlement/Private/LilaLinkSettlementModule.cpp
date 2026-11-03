#include "LilaLinkSettlement.h"

#include "LilaLinkMethodRegistry.h"

#include "Modules/ModuleManager.h"

class FLilaLinkSettlementModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		LilaLinkSettlement::RegisterMethods();
	}

	virtual void ShutdownModule() override
	{
		LilaLink::UnregisterFeature(LilaLinkSettlement::Feature);
	}
};

IMPLEMENT_MODULE(FLilaLinkSettlementModule, LilaLinkSettlement)
