#include "LilaLinkMethodRegistry.h"

#include "LilaLinkServer.h"

#include "Misc/ScopeLock.h"

namespace
{
struct FRegistered
{
	LilaLink::FExternalMethod Method;
	FString Feature;
};

FCriticalSection& Lock()
{
	static FCriticalSection Section;
	return Section;
}

TMap<FString, FRegistered>& Registry()
{
	static TMap<FString, FRegistered> Methods;
	return Methods;
}
} // namespace

void LilaLink::RegisterMethod(const FString& Name, FExternalMethod Method, const FString& Feature)
{
	FScopeLock Guard(&Lock());
	Registry().Add(Name, {MoveTemp(Method), Feature});
}

void LilaLink::UnregisterFeature(const FString& Feature)
{
	FScopeLock Guard(&Lock());
	for (auto It = Registry().CreateIterator(); It; ++It)
	{
		if (It->Value.Feature == Feature)
		{
			It.RemoveCurrent();
		}
	}
}

TArray<FString> LilaLink::ExternalFeatures()
{
	FScopeLock Guard(&Lock());
	TArray<FString> Out;
	for (const TPair<FString, FRegistered>& Entry : Registry())
	{
		if (!Entry.Value.Feature.IsEmpty())
		{
			Out.AddUnique(Entry.Value.Feature);
		}
	}
	return Out;
}

bool LilaLink::FindExternalMethod(const FString& Name, FLilaLinkMethod& Out)
{
	FScopeLock Guard(&Lock());
	const FRegistered* Found = Registry().Find(Name);
	if (!Found)
	{
		return false;
	}
	Out = LilaLink::Sync(Found->Method);
	return true;
}
