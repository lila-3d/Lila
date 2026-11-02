#include "LilaComponentUserData.h"

#include "Engine/DataAsset.h"
#include "Interfaces/Interface_AssetUserData.h"

bool ULilaComponentUserData::GetValue(const FString& Component, FName Attribute, FString& OutValue) const
{
	const FLilaComponentValues* Values = Components.Find(Component);
	const FString* Value = Values ? Values->Values.Find(Attribute) : nullptr;
	if (!Value)
	{
		return false;
	}
	OutValue = *Value;
	return true;
}

double ULilaComponentUserData::GetNumber(const FString& Component, FName Attribute, double Fallback) const
{
	FString Value;
	if (!GetValue(Component, Attribute, Value))
	{
		return Fallback;
	}
	if (Value == TEXT("true"))
	{
		return 1.0;
	}
	if (Value == TEXT("false"))
	{
		return 0.0;
	}
	return Value.IsNumeric() ? FCString::Atod(*Value) : Fallback;
}

UDataAsset* ULilaComponentUserData::GetDataAsset(const FString& Component) const
{
	const FLilaComponentValues* Values = Components.Find(Component);
	return Values ? Values->DataAsset.Get() : nullptr;
}

ULilaComponentUserData* ULilaComponentUserData::FindOn(UObject* Owner)
{
	IInterface_AssetUserData* Holder = Cast<IInterface_AssetUserData>(Owner);
	return Holder ? Holder->GetAssetUserData<ULilaComponentUserData>() : nullptr;
}
