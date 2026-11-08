#include "LilaLinkPCG.h"

#include "LilaLinkMethodRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SplineComponent.h"
#include "Curve/GeneralPolygon2.h"
#include "Data/PCGPointArrayData.h"
#include "Data/PCGPolygon2DData.h"
#include "Data/PCGSplineData.h"
#include "Dom/JsonObject.h"
#include "Helpers/PCGHelpers.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Misc/PackageName.h"
#include "PCGDataAsset.h"
#include "Polygon2.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace
{
bool ReadVec(const TSharedPtr<FJsonValue>& Value, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Xyz = nullptr;
	if (!Value.IsValid() || !Value->TryGetArray(Xyz) || Xyz->Num() != 3)
	{
		return false;
	}
	Out = FVector((*Xyz)[0]->AsNumber(), (*Xyz)[1]->AsNumber(), (*Xyz)[2]->AsNumber());
	return true;
}

FVector VecOr(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FVector& Default)
{
	FVector V;
	return ReadVec(Object->TryGetField(Field), V) ? V : Default;
}

FQuat ReadQuat(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Q = nullptr;
	if (!Object->TryGetArrayField(Field, Q) || Q->Num() != 4)
	{
		return FQuat::Identity;
	}
	return FQuat((*Q)[0]->AsNumber(), (*Q)[1]->AsNumber(), (*Q)[2]->AsNumber(), (*Q)[3]->AsNumber()).GetNormalized();
}

TArray<FVector> ReadPath(const TSharedPtr<FJsonObject>& Object)
{
	TArray<FVector> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Object->TryGetArrayField(TEXT("path"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			FVector P;
			if (ReadVec(V, P))
			{
				Out.Add(P);
			}
		}
	}
	return Out;
}

/** Number, bool and string fields of Attributes as attributes of the data's own domain. */
void SetDataAttributes(UPCGData* Data, const TSharedPtr<FJsonObject>& Attributes)
{
	UPCGMetadata* Metadata = Data && Attributes.IsValid() ? Data->MutableMetadata() : nullptr;
	if (!Metadata)
	{
		return;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Attributes->Values)
	{
		if (!Field.Value.IsValid())
		{
			continue;
		}
		const FPCGAttributeIdentifier Name(FName(*Field.Key), PCGMetadataDomainID::Data);
		switch (Field.Value->Type)
		{
		case EJson::Number:
			Metadata->FindOrCreateAttribute<double>(Name, Field.Value->AsNumber(), false, false);
			break;
		case EJson::Boolean:
			Metadata->FindOrCreateAttribute<bool>(Name, Field.Value->AsBool(), false, false);
			break;
		case EJson::String:
			Metadata->FindOrCreateAttribute<FString>(Name, Field.Value->AsString(), false, false);
			break;
		default:
			break;
		}
	}
}

void SetPointAttribute(UPCGMetadata* Metadata, int64 Key, const FString& Name, const TSharedPtr<FJsonValue>& Value)
{
	const FName Id(*Name);
	FVector V;
	switch (Value->Type)
	{
	case EJson::Number:
		Metadata->FindOrCreateAttribute<double>(Id, 0.0, true, false)->SetValue(Key, Value->AsNumber());
		break;
	case EJson::Boolean:
		Metadata->FindOrCreateAttribute<bool>(Id, false, false, false)->SetValue(Key, Value->AsBool());
		break;
	case EJson::String:
		Metadata->FindOrCreateAttribute<FString>(Id, FString(), false, false)->SetValue(Key, Value->AsString());
		break;
	case EJson::Array:
		if (ReadVec(Value, V))
		{
			Metadata->FindOrCreateAttribute<FVector>(Id, FVector::ZeroVector, true, false)->SetValue(Key, V);
		}
		break;
	default:
		break;
	}
}

UPCGPointArrayData* MakePoints(UObject* Outer, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPCG::MakePoints);

	UPCGPointArrayData* Points = NewObject<UPCGPointArrayData>(Outer);
	Points->SetNumPoints(Entries.Num());
	UPCGMetadata* Metadata = Points->MutableMetadata();
	auto Transforms = Points->GetTransformValueRange();
	auto Density = Points->GetDensityValueRange();
	auto BoundsMin = Points->GetBoundsMinValueRange();
	auto BoundsMax = Points->GetBoundsMaxValueRange();
	auto Seeds = Points->GetSeedValueRange();
	auto Keys = Points->GetMetadataEntryValueRange();
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> E = Entries[Index].IsValid() ? Entries[Index]->AsObject() : nullptr;
		if (!E.IsValid())
		{
			continue;
		}
		const FVector Location = VecOr(E, TEXT("location"), FVector::ZeroVector);
		Transforms[Index] = FTransform(ReadQuat(E, TEXT("rotation")), Location, VecOr(E, TEXT("scale"), FVector::OneVector));
		double D = 1.0;
		E->TryGetNumberField(TEXT("density"), D);
		Density[Index] = float(D);
		BoundsMin[Index] = VecOr(E, TEXT("boundsMin"), FVector(-50.0));
		BoundsMax[Index] = VecOr(E, TEXT("boundsMax"), FVector(50.0));
		// Seeded from the position, as PCG's own samplers do, so a resend keeps each point's random choices.
		Seeds[Index] = PCGHelpers::ComputeSeedFromPosition(Location);
		const int64 Key = Metadata->AddEntry();
		Keys[Index] = Key;
		const TSharedPtr<FJsonObject>* Attributes = nullptr;
		if (E->TryGetObjectField(TEXT("attributes"), Attributes))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Attributes)->Values)
			{
				if (Field.Value.IsValid())
				{
					SetPointAttribute(Metadata, Key, Field.Key, Field.Value);
				}
			}
		}
	}
	return Points;
}

UPCGSplineData* MakeSpline(UObject* Outer, const TArray<FVector>& Path, bool bClosed)
{
	if (Path.Num() < 2)
	{
		return nullptr;
	}
	TArray<FSplinePoint> SplinePoints;
	for (int32 Index = 0; Index < Path.Num(); ++Index)
	{
		SplinePoints.Emplace(static_cast<float>(Index), Path[Index], ESplinePointType::Linear);
	}
	UPCGSplineData* Data = NewObject<UPCGSplineData>(Outer);
	Data->Initialize(SplinePoints, bClosed, FTransform::Identity);
	return Data;
}

UPCGPolygon2DData* MakePolygon(UObject* Outer, const TArray<FVector>& Path)
{
	if (Path.Num() < 3)
	{
		return nullptr;
	}
	UE::Geometry::FPolygon2d Outline;
	for (const FVector& P : Path)
	{
		Outline.AppendVertex(FVector2d(P.X, P.Y));
	}
	UPCGPolygon2DData* Data = NewObject<UPCGPolygon2DData>(Outer);
	Data->SetPolygon(UE::Geometry::FGeneralPolygon2d(Outline));
	Data->SetTransform(FTransform(FVector(0.0, 0.0, Path[0].Z)), true);
	return Data;
}

UPCGDataAsset* FindOrCreateDataAsset(const FString& AssetPath, FString& OutError)
{
	const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	if (UPCGDataAsset* Existing = LoadObject<UPCGDataAsset>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		return Existing;
	}
	if (!FPackageName::IsValidLongPackageName(AssetPath))
	{
		OutError = FString::Printf(TEXT("setPcgData: %s is not a content path"), *AssetPath);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*AssetPath);
	UPCGDataAsset* Asset = NewObject<UPCGDataAsset>(Package, *FPackageName::GetShortName(AssetPath), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	return Asset;
}

TSharedPtr<FJsonValue> SetPcgData(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPCG::SetPcgData);

	FString AssetPath, Label;
	if (!Params->TryGetStringField(TEXT("asset"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("setPcgData: give asset");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("label"), Label);
	const TArray<TSharedPtr<FJsonValue>>* Sets = nullptr;
	if (!Params->TryGetArrayField(TEXT("sets"), Sets))
	{
		OutError = TEXT("setPcgData: give sets");
		return nullptr;
	}
	UPCGDataAsset* Asset = FindOrCreateDataAsset(AssetPath, OutError);
	if (!Asset)
	{
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetPcgData", "Lila: Update PCG Data"));
	Asset->Modify();
	Asset->Data.TaggedData.Reset();
	Asset->Name = Label;
#if WITH_EDITORONLY_DATA
	Asset->Description = FText::FromString(TEXT("Data from Lila."));
#endif
	int32 Written = 0, PointCount = 0;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	for (const TSharedPtr<FJsonValue>& SetValue : *Sets)
	{
		const TSharedPtr<FJsonObject> Set = SetValue.IsValid() ? SetValue->AsObject() : nullptr;
		if (!Set.IsValid())
		{
			continue;
		}
		FString Kind = TEXT("points"), Pin, Tag;
		Set->TryGetStringField(TEXT("kind"), Kind);
		Set->TryGetStringField(TEXT("pin"), Pin);
		Set->TryGetStringField(TEXT("tag"), Tag);
		UPCGData* Data = nullptr;
		if (Kind == TEXT("points"))
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (Set->TryGetArrayField(TEXT("points"), Entries))
			{
				Data = MakePoints(Asset, *Entries);
				PointCount += Entries->Num();
			}
		}
		else if (Kind == TEXT("spline"))
		{
			bool bClosed = false;
			Set->TryGetBoolField(TEXT("closed"), bClosed);
			Data = MakeSpline(Asset, ReadPath(Set), bClosed);
		}
		else if (Kind == TEXT("polygon"))
		{
			Data = MakePolygon(Asset, ReadPath(Set));
		}
		if (!Data)
		{
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("set on pin %s (%s) had too few points or an unknown kind"), *Pin, *Kind)));
			continue;
		}
		const TSharedPtr<FJsonObject>* Attributes = nullptr;
		if (Set->TryGetObjectField(TEXT("attributes"), Attributes))
		{
			SetDataAttributes(Data, *Attributes);
		}
		FPCGTaggedData& Tagged = Asset->Data.TaggedData.AddDefaulted_GetRef();
		Tagged.Data = Data;
		Tagged.Pin = FName(Pin.IsEmpty() ? TEXT("Out") : *Pin);
		if (!Tag.IsEmpty())
		{
			Tagged.Tags.Add(Tag);
		}
		++Written;
	}
	Asset->MarkPackageDirty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset"), Asset->GetPathName());
	Out->SetNumberField(TEXT("sets"), Written);
	Out->SetNumberField(TEXT("points"), PointCount);
	Out->SetArrayField(TEXT("warnings"), Warnings);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLinkPCG::RegisterMethods()
{
	LilaLink::RegisterMethod(TEXT("setPcgData"), &SetPcgData, Feature);
}
