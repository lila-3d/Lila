#include "LilaInstanceData.h"

#include "GameFramework/Actor.h"

bool ULilaInstanceData::GetValue(int32 InstanceIndex, FName Attribute, FString& OutValue) const
{
	const FLilaInstanceColumn* Column = Columns.Find(Attribute);
	if (!Column || !Column->Values.IsValidIndex(InstanceIndex))
	{
		return false;
	}
	OutValue = Column->Values[InstanceIndex];
	return true;
}

double ULilaInstanceData::GetNumber(int32 InstanceIndex, FName Attribute, double Fallback) const
{
	FString Value;
	if (!GetValue(InstanceIndex, Attribute, Value))
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

int32 ULilaInstanceData::FindInstance(const FString& Id) const
{
	return InstanceIds.IndexOfByKey(Id);
}

int32 ULilaInstanceData::FindCustomDataSlot(FName Name) const
{
	const int32 Slot = CustomDataNames.IndexOfByKey(Name);
	return Slot != INDEX_NONE ? Slot : CustomDataNames.IndexOfByKey(FName(*(Name.ToString() + TEXT(".0"))));
}

ULilaInstanceData* ULilaInstanceData::FindOn(const AActor* Actor)
{
	return Actor ? Actor->FindComponentByClass<ULilaInstanceData>() : nullptr;
}
