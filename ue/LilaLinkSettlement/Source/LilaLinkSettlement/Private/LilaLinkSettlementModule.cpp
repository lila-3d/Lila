#include "LilaLinkSettlement.h"

#include "LilaLinkMethodRegistry.h"

#include "Modules/ModuleManager.h"

class FLilaLinkSettlementModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		LilaLinkSettlement::RegisterMethods();
		LilaLinkSettlement::RegisterCellMethods();
	}

	virtual void ShutdownModule() override
	{
		LilaLink::UnregisterFeature(LilaLinkSettlement::Feature);
		LilaLink::UnregisterFeature(LilaLinkSettlement::CellsFeature);
	}
};

IMPLEMENT_MODULE(FLilaLinkSettlementModule, LilaLinkSettlement)
