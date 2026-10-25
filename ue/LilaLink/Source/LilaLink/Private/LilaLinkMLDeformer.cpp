#include "LilaLinkMLDeformer.h"

#include "LilaLinkLog.h"
#include "LilaLinkMethods.h"

#include "AssetToolsModule.h"
#include "Factories/Factory.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
FString ToObjectPath(const FString& Path)
{
	return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
}

UObject* LoadAsset(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutError)
{
	FString Path;
	if (!Params->TryGetStringField(Field, Path) || Path.IsEmpty())
	{
		OutError = FString::Printf(TEXT("createMLDeformer: give %s"), Field);
		return nullptr;
	}
	UObject* Asset = LoadObject<UObject>(nullptr, *ToObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("createMLDeformer: no asset at %s"), *Path);
	}
	return Asset;
}

/** Sets an object or soft object property by name; false when the container's struct has no such property. */
bool SetObjectField(const UStruct* Struct, void* Container, const TCHAR* Name, UObject* Value)
{
	FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(Struct->FindPropertyByName(Name));
	if (!Prop)
	{
		return false;
	}
	Prop->SetObjectPropertyValue_InContainer(Container, Value);
	return true;
}

UObject* GetObjectField(const UStruct* Struct, const void* Container, const TCHAR* Name)
{
	const FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(Struct->FindPropertyByName(Name));
	return Prop ? Prop->GetObjectPropertyValue_InContainer(Container) : nullptr;
}

void NotifyChanged(UObject* Object, FProperty* Prop)
{
	FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(Event);
}

TSharedPtr<FJsonValue> CreateMLDeformer(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMLDeformer::CreateMLDeformer);

	// Found by name, so projects without the ML Deformer plugins still load Lila Link.
	UClass* FactoryClass = FindObject<UClass>(FTopLevelAssetPath(TEXT("/Script/MLDeformerFrameworkEditor"), TEXT("MLDeformerFactory")));
	UClass* AssetClass = FindObject<UClass>(FTopLevelAssetPath(TEXT("/Script/MLDeformerFramework"), TEXT("MLDeformerAsset")));
	if (!FactoryClass || !AssetClass)
	{
		OutError = TEXT("createMLDeformer: enable the ML Deformer Framework plugin and a model plugin (Neural Morph Model) in this project");
		return nullptr;
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || !Path.StartsWith(TEXT("/")))
	{
		OutError = TEXT("createMLDeformer: give path, like /Game/Chars/MLD_Hero");
		return nullptr;
	}
	UObject* SkeletalMesh = LoadAsset(Params, TEXT("skeletalMesh"), OutError);
	UObject* AnimSequence = SkeletalMesh ? LoadAsset(Params, TEXT("animSequence"), OutError) : nullptr;
	UObject* GeometryCache = AnimSequence ? LoadAsset(Params, TEXT("geometryCache"), OutError) : nullptr;
	if (!GeometryCache)
	{
		return nullptr;
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *ToObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Asset && !Asset->IsA(AssetClass))
	{
		OutError = FString::Printf(TEXT("createMLDeformer: %s is a %s, not an ML Deformer"), *Path, *Asset->GetClass()->GetName());
		return nullptr;
	}
	if (!Asset)
	{
		UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		Asset = Tools.CreateAsset(FPackageName::GetShortName(Path), FPackageName::GetLongPackagePath(Path), AssetClass, Factory);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("createMLDeformer: could not create %s"), *Path);
			return nullptr;
		}
	}

	FObjectProperty* ModelProp = CastField<FObjectProperty>(AssetClass->FindPropertyByName(TEXT("Model")));
	UObject* Model = ModelProp ? ModelProp->GetObjectPropertyValue_InContainer(Asset) : nullptr;
	if (!Model)
	{
		OutError = TEXT("createMLDeformer: the asset has no model; enable a model plugin (Neural Morph Model, Nearest Neighbor Model or Vertex Delta Model)");
		return nullptr;
	}
	UClass* ModelClass = Model->GetClass();
	FArrayProperty* InputsProp = CastField<FArrayProperty>(ModelClass->FindPropertyByName(TEXT("TrainingInputAnims")));
	FStructProperty* InputProp = InputsProp ? CastField<FStructProperty>(InputsProp->Inner) : nullptr;
	FProperty* MeshProp = ModelClass->FindPropertyByName(TEXT("SkeletalMesh"));
	if (!InputProp || !MeshProp)
	{
		OutError = FString::Printf(TEXT("createMLDeformer: the %s model doesn't train from a geometry cache"), *ModelClass->GetName());
		return nullptr;
	}

	Asset->Modify();
	Model->Modify();
	Model->PreEditChange(MeshProp);
	SetObjectField(ModelClass, Model, TEXT("SkeletalMesh"), SkeletalMesh);
	NotifyChanged(Model, MeshProp);

	Model->PreEditChange(InputsProp);
	FScriptArrayHelper Inputs(InputsProp, InputsProp->ContainerPtrToValuePtr<void>(Model));
	const UScriptStruct* InputStruct = InputProp->Struct;
	int32 Index = INDEX_NONE;
	for (int32 I = 0; I < Inputs.Num(); ++I)
	{
		if (GetObjectField(InputStruct, Inputs.GetRawPtr(I), TEXT("AnimSequence")) == AnimSequence)
		{
			Index = I;
			break;
		}
	}
	if (Index == INDEX_NONE)
	{
		Index = Inputs.AddValue();
	}
	void* Input = Inputs.GetRawPtr(Index);
	SetObjectField(InputStruct, Input, TEXT("AnimSequence"), AnimSequence);
	if (!SetObjectField(InputStruct, Input, TEXT("GeometryCache"), GeometryCache))
	{
		OutError = TEXT("createMLDeformer: the training input has no GeometryCache field");
		return nullptr;
	}
	if (FBoolProperty* Enabled = CastField<FBoolProperty>(InputStruct->FindPropertyByName(TEXT("bEnabled"))))
	{
		Enabled->SetPropertyValue_InContainer(Input, true);
	}
	NotifyChanged(Model, InputsProp);
	Asset->MarkPackageDirty();
	UE_LOG(LogLilaLink, Log, TEXT("createMLDeformer: %s (%s) trains on %s"), *Asset->GetPathName(), *ModelClass->GetName(), *AnimSequence->GetPathName());

	TArray<TSharedPtr<FJsonValue>> SaveErrors;
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	if (bSave)
	{
		TArray<FString> Errors;
		LilaLink::SaveAssetPaths({Asset->GetPathName()}, Errors);
		for (const FString& E : Errors)
		{
			SaveErrors.Add(MakeShared<FJsonValueString>(E));
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset"), Asset->GetPathName());
	Out->SetStringField(TEXT("model"), ModelClass->GetName());
	Out->SetNumberField(TEXT("trainingInputs"), Inputs.Num());
	Out->SetArrayField(TEXT("errors"), SaveErrors);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddMLDeformerMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("createMLDeformer"), LilaLink::Sync(&CreateMLDeformer));
}
