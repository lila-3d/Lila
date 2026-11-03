#include "LilaLinkComponents.h"

#include "LilaComponentExporter.h"
#include "LilaComponentUserData.h"
#include "LilaLinkLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "GameFramework/Actor.h"
#include "IPythonScriptPlugin.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "PythonScriptTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace
{
const TCHAR* PythonModule = TEXT("lila_link_components");

TMap<FName, TSharedRef<ILilaComponentExporter>>& Exporters()
{
	static TMap<FName, TSharedRef<ILilaComponentExporter>> Map;
	return Map;
}

IPythonScriptPlugin* Python()
{
	IPythonScriptPlugin* P = IPythonScriptPlugin::Get();
	return P && P->IsPythonAvailable() ? P : nullptr;
}

/** A Python string literal holding S. */
FString PyString(const FString& S)
{
	FString Out = TEXT("'");
	for (TCHAR C : S)
	{
		switch (C)
		{
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('\''): Out += TEXT("\\'"); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\r'): Out += TEXT("\\r"); break;
		default: Out.AppendChar(C);
		}
	}
	return Out + TEXT("'");
}

FString JsonText(const TSharedRef<FJsonObject>& Object)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object, Writer);
	return Out;
}

/** A value as user data and tags hold it: numbers without a needless fraction, true/false, lists comma-separated. */
FString ValueText(const TSharedPtr<FJsonValue>& V)
{
	if (!V.IsValid())
	{
		return FString();
	}
	switch (V->Type)
	{
	case EJson::String:
		return V->AsString();
	case EJson::Boolean:
		return V->AsBool() ? TEXT("true") : TEXT("false");
	case EJson::Number:
	{
		const double D = V->AsNumber();
		return FMath::Abs(D) < 1e15 && D == FMath::RoundToDouble(D) ? FString::Printf(TEXT("%lld"), static_cast<long long>(D)) : FString::SanitizeFloat(D);
	}
	case EJson::Array:
	{
		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& Item : V->AsArray())
		{
			Parts.Add(ValueText(Item));
		}
		return FString::Join(Parts, TEXT(","));
	}
	case EJson::Object:
		return JsonText(V->AsObject().ToSharedRef());
	default:
		return FString();
	}
}

bool NumberOf(const TSharedPtr<FJsonValue>& V, double& Out)
{
	bool B = false;
	FString S;
	if (V->TryGetNumber(Out))
	{
		return true;
	}
	if (V->TryGetBool(B))
	{
		Out = B ? 1.0 : 0.0;
		return true;
	}
	if (V->TryGetString(S) && S.IsNumeric())
	{
		Out = FCString::Atod(*S);
		return true;
	}
	return false;
}

bool NumbersOf(const TSharedPtr<FJsonValue>& V, TArray<double>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!V->TryGetArray(Items))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Item : *Items)
	{
		double D = 0.0;
		if (!Item.IsValid() || !NumberOf(Item, D))
		{
			return false;
		}
		Out.Add(D);
	}
	return true;
}

/** An enum value from its label (short name or display name, any case) or its index. */
bool EnumValueOf(const UEnum* Enum, const TSharedPtr<FJsonValue>& V, int64& Out)
{
	const int32 Count = Enum->ContainsExistingMax() ? Enum->NumEnums() - 1 : Enum->NumEnums();
	FString Label;
	if (V->TryGetString(Label))
	{
		for (int32 I = 0; I < Count; ++I)
		{
			if (Enum->GetNameStringByIndex(I).Equals(Label, ESearchCase::IgnoreCase) || Enum->GetDisplayNameTextByIndex(I).ToString().Equals(Label, ESearchCase::IgnoreCase))
			{
				Out = Enum->GetValueByIndex(I);
				return true;
			}
		}
		return false;
	}
	double D = 0.0;
	if (NumberOf(V, D) && D >= 0.0 && int32(D) < Count)
	{
		Out = Enum->GetValueByIndex(int32(D));
		return true;
	}
	return false;
}

/** Sets the value at Ptr from V. False with OutError when the value does not fit the property. */
bool SetValue(const FProperty* Prop, void* Ptr, const TSharedPtr<FJsonValue>& V, UObject* Owner, FString& OutError)
{
	if (!V.IsValid() || V->IsNull())
	{
		OutError = TEXT("no value");
		return false;
	}
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(Prop))
	{
		double D = 0.0;
		const FString S = ValueText(V);
		Bool->SetPropertyValue(Ptr, S.Equals(TEXT("true"), ESearchCase::IgnoreCase) || (NumberOf(V, D) && D != 0.0));
		return true;
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		int64 Value = 0;
		if (!EnumValueOf(EnumProp->GetEnum(), V, Value))
		{
			OutError = FString::Printf(TEXT("%s is not a value of %s"), *ValueText(V), *EnumProp->GetEnum()->GetName());
			return false;
		}
		EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(Ptr, Value);
		return true;
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Prop))
	{
		if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
		{
			int64 Value = 0;
			if (!EnumValueOf(Enum, V, Value))
			{
				OutError = FString::Printf(TEXT("%s is not a value of %s"), *ValueText(V), *Enum->GetName());
				return false;
			}
			Numeric->SetIntPropertyValue(Ptr, Value);
			return true;
		}
		double D = 0.0;
		if (!NumberOf(V, D))
		{
			OutError = FString::Printf(TEXT("%s is not a number"), *ValueText(V));
			return false;
		}
		if (Numeric->IsFloatingPoint())
		{
			Numeric->SetFloatingPointPropertyValue(Ptr, D);
		}
		else
		{
			Numeric->SetIntPropertyValue(Ptr, static_cast<int64>(FMath::RoundToDouble(D)));
		}
		return true;
	}
	if (const FStrProperty* Str = CastField<FStrProperty>(Prop))
	{
		Str->SetPropertyValue(Ptr, ValueText(V));
		return true;
	}
	if (const FNameProperty* Name = CastField<FNameProperty>(Prop))
	{
		Name->SetPropertyValue(Ptr, FName(*ValueText(V)));
		return true;
	}
	if (const FTextProperty* Text = CastField<FTextProperty>(Prop))
	{
		Text->SetPropertyValue(Ptr, FText::FromString(ValueText(V)));
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Prop))
	{
		TArray<double> C;
		if (NumbersOf(V, C))
		{
			const UScriptStruct* S = Struct->Struct;
			if (S == TBaseStructure<FVector>::Get() && C.Num() == 3)
			{
				*static_cast<FVector*>(Ptr) = FVector(C[0], C[1], C[2]);
				return true;
			}
			if (S == TBaseStructure<FVector2D>::Get() && C.Num() >= 2)
			{
				*static_cast<FVector2D*>(Ptr) = FVector2D(C[0], C[1]);
				return true;
			}
			if ((S == TBaseStructure<FLinearColor>::Get() || S == TBaseStructure<FColor>::Get()) && (C.Num() == 3 || C.Num() == 4))
			{
				const FLinearColor Color(float(C[0]), float(C[1]), float(C[2]), C.Num() == 4 ? float(C[3]) : 1.0f);
				if (S == TBaseStructure<FColor>::Get())
				{
					*static_cast<FColor*>(Ptr) = Color.ToFColor(true);
				}
				else
				{
					*static_cast<FLinearColor*>(Ptr) = Color;
				}
				return true;
			}
		}
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Prop))
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (V->TryGetArray(Items))
		{
			FScriptArrayHelper Helper(Array, Ptr);
			Helper.Resize(Items->Num());
			for (int32 I = 0; I < Items->Num(); ++I)
			{
				if (!SetValue(Array->Inner, Helper.GetRawPtr(I), (*Items)[I], Owner, OutError))
				{
					OutError = FString::Printf(TEXT("element %d: %s"), I, *OutError);
					return false;
				}
			}
			return true;
		}
	}
	if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(Prop))
	{
		Soft->SetPropertyValue(Ptr, FSoftObjectPtr(FSoftObjectPath(ValueText(V))));
		return true;
	}
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Prop))
	{
		const FString Path = ValueText(V);
		UObject* Found = Path.IsEmpty() ? nullptr : StaticLoadObject(Object->PropertyClass, nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Found && !Path.IsEmpty())
		{
			OutError = FString::Printf(TEXT("no %s at %s"), *Object->PropertyClass->GetName(), *Path);
			return false;
		}
		Object->SetObjectPropertyValue(Ptr, Found);
		return true;
	}
	// Anything else takes Unreal's text form, like (Pitch=0,Yaw=90,Roll=0) for a rotator.
	FString Text;
	if (V->TryGetString(Text) && Prop->ImportText_Direct(*Text, Ptr, Owner, PPF_None) != nullptr)
	{
		return true;
	}
	OutError = FString::Printf(TEXT("can't set a %s from %s"), *Prop->GetCPPType(), *ValueText(V));
	return false;
}

/** The property an attribute names: the whole name, the part after a namespace (health:max), or a bool's bName. */
FProperty* MatchingProperty(const UStruct* Struct, const FString& Attr)
{
	int32 Colon = INDEX_NONE;
	const FString Short = Attr.FindLastChar(TEXT(':'), Colon) ? Attr.RightChop(Colon + 1) : Attr;
	for (const FString& Candidate : {Attr, Short, FString(TEXT("b")) + Short})
	{
		if (FProperty* Prop = FindFProperty<FProperty>(Struct, FName(*Candidate)))
		{
			return Prop;
		}
	}
	return nullptr;
}

/** Sets Target's properties named like the component's attributes; names with no property are reported together. */
void SetProperties(UObject* Target, const LilaLink::FLilaComponent& C, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkComponents::SetProperties);

	Target->Modify();
	TArray<FString> Unmatched;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Attr : C.Attrs->Values)
	{
		FProperty* Prop = MatchingProperty(Target->GetClass(), Attr.Key);
		if (!Prop)
		{
			Unmatched.Add(Attr.Key);
			continue;
		}
		FString Error;
		if (!SetValue(Prop, Prop->ContainerPtrToValuePtr<void>(Target), Attr.Value, Target, Error))
		{
			OutWarnings.Add(FString::Printf(TEXT("%s: %s.%s: %s"), *C.Name, *Target->GetClass()->GetName(), *Prop->GetName(), *Error));
		}
	}
	if (!Unmatched.IsEmpty())
	{
		OutWarnings.Add(FString::Printf(TEXT("%s: %s has no property named %s"), *C.Name, *Target->GetClass()->GetName(), *FString::Join(Unmatched, TEXT(", "))));
	}
	Target->PostEditChange();
}

/** A class from /Script/Module.Class, a Blueprint asset or class path, or a short class name; must derive from Base. */
UClass* ResolveClass(const FString& Name, UClass* Base, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkComponents::ResolveClass);

	FString Path = Name.TrimStartAndEnd();
	if (Path.IsEmpty())
	{
		OutError = FString::Printf(TEXT("no class given; set one in the component's Unreal export (a %s)"), *Base->GetName());
		return nullptr;
	}
	if (!Path.StartsWith(TEXT("/")))
	{
		const FTopLevelAssetPath Full = UClass::TryConvertShortTypeNameToPathName<UClass>(Path);
		if (Full.IsValid())
		{
			Path = Full.ToString();
		}
	}
	UClass* Class = FindObject<UClass>(nullptr, *Path);
	if (!Class && Path.StartsWith(TEXT("/")))
	{
		Class = LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (!Class && Path.StartsWith(TEXT("/")) && !Path.StartsWith(TEXT("/Script/")))
	{
		const FString Object = Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
		if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Object, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Class = Blueprint->GeneratedClass;
		}
		else if (!Object.EndsWith(TEXT("_C")))
		{
			Class = LoadObject<UClass>(nullptr, *(Object + TEXT("_C")), nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
	}
	if (!Class)
	{
		OutError = FString::Printf(TEXT("no class %s"), *Name);
		return nullptr;
	}
	if (!Class->IsChildOf(Base))
	{
		OutError = FString::Printf(TEXT("%s is not a %s"), *Class->GetPathName(), *Base->GetName());
		return nullptr;
	}
	if (Class->HasAnyClassFlags(CLASS_Abstract))
	{
		OutError = FString::Printf(TEXT("%s is abstract"), *Class->GetPathName());
		return nullptr;
	}
	return Class;
}

FLilaComponentValues ValuesOf(const LilaLink::FLilaComponent& C)
{
	FLilaComponentValues Out;
	Out.Mode = C.Mode;
	Out.ClassPath = C.Class;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Attr : C.Attrs->Values)
	{
		Out.Values.Add(FName(*Attr.Key), ValueText(Attr.Value));
	}
	return Out;
}

FString ComponentLabel(const FString& Name) { return Name.IsEmpty() ? FString(TEXT("Attributes")) : Name; }

/** Makes or updates the data asset DA_<owner>_<component> of the component's class in Folder. */
UDataAsset* MakeDataAsset(const LilaLink::FLilaComponent& C, const FString& Folder, const FString& OwnerName, TArray<FString>& OutWarnings,
	TArray<FString>& OutAssets)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkComponents::MakeDataAsset);

	FString Error;
	UClass* Class = ResolveClass(C.Class, UDataAsset::StaticClass(), Error);
	if (!Class)
	{
		OutWarnings.Add(FString::Printf(TEXT("%s: %s"), *C.Name, *Error));
		return nullptr;
	}
	const FString AssetName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("DA_%s_%s"), *OwnerName, *ComponentLabel(C.Name)));
	const FString PackageName = Folder / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
	if (!Existing && FPackageName::DoesPackageExist(PackageName))
	{
		Existing = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	UDataAsset* Asset = Cast<UDataAsset>(Existing);
	if (Existing && (!Asset || !Asset->IsA(Class)))
	{
		OutWarnings.Add(FString::Printf(TEXT("%s: %s is not a %s; left as it is"), *C.Name, *ObjectPath, *Class->GetName()));
		return nullptr;
	}
	if (!Asset)
	{
		UPackage* Package = CreatePackage(*PackageName);
		Asset = NewObject<UDataAsset>(Package, Class, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
	}
	SetProperties(Asset, C, OutWarnings);
	Asset->MarkPackageDirty();
	OutAssets.AddUnique(Asset->GetPathName());
	return Asset;
}

/** Replaces Owner's Lila user data with Components and Tags, or removes it when both are empty. */
void WriteUserData(UObject* Owner, TMap<FString, FLilaComponentValues>&& Components, TArray<FName>&& Tags)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkComponents::WriteUserData);

	IInterface_AssetUserData* Holder = Cast<IInterface_AssetUserData>(Owner);
	if (!Holder)
	{
		return;
	}
	ULilaComponentUserData* Data = Holder->GetAssetUserData<ULilaComponentUserData>();
	if (Components.IsEmpty() && Tags.IsEmpty())
	{
		if (Data)
		{
			Owner->Modify();
			Holder->RemoveUserDataOfClass(ULilaComponentUserData::StaticClass());
		}
		return;
	}
	Owner->Modify();
	if (!Data)
	{
		Data = NewObject<ULilaComponentUserData>(Owner, NAME_None, RF_Transactional);
		Holder->AddAssetUserData(Data);
	}
	Data->Modify();
	Data->Components = MoveTemp(Components);
	Data->ActorTags = MoveTemp(Tags);
}

/** custom: a C++ exporter of that name, else a Python one registered through lila_link_components. */
void RunExporter(const LilaLink::FLilaComponent& C, AActor* Actor, UObject* Asset, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkComponents::RunExporter);

	if (const TSharedRef<ILilaComponentExporter>* Exporter = Exporters().Find(FName(*C.Class)))
	{
		if (!(*Exporter)->Apply(Actor, Asset, C.Name, C.Attrs, OutWarnings))
		{
			OutWarnings.Add(FString::Printf(TEXT("%s: exporter %s failed"), *C.Name, *C.Class));
		}
		return;
	}
	IPythonScriptPlugin* Py = Python();
	if (!Py)
	{
		OutWarnings.Add(FString::Printf(TEXT("%s: no component exporter %s is registered"), *C.Name, *C.Class));
		return;
	}
	FPythonCommandEx Command;
	Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	Command.Command = FString::Printf(TEXT("import %s\n%s._apply(%s, %s, %s, %s, %s)\n"), PythonModule, PythonModule, *PyString(C.Class),
		*PyString(Actor ? Actor->GetPathName() : FString()), *PyString(Asset ? Asset->GetPathName() : FString()), *PyString(C.Name), *PyString(JsonText(C.Attrs)));
	if (!Py->ExecPythonCommandEx(Command))
	{
		TArray<FString> Errors;
		for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
		{
			if (Entry.Type == EPythonLogOutputType::Error)
			{
				Errors.Add(Entry.Output.TrimStartAndEnd());
			}
		}
		OutWarnings.Add(FString::Printf(TEXT("%s: exporter %s: %s"), *C.Name, *C.Class, Errors.IsEmpty() ? *Command.CommandResult : *FString::Join(Errors, TEXT(" "))));
	}
}

/** component: the actor's component of the class, added when it has none, with its properties set. */
void ApplyActorComponent(AActor* Actor, const LilaLink::FLilaComponent& C, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkComponents::ApplyActorComponent);

	FString Error;
	UClass* Class = ResolveClass(C.Class, UActorComponent::StaticClass(), Error);
	if (!Class)
	{
		OutWarnings.Add(FString::Printf(TEXT("%s: %s"), *C.Name, *Error));
		return;
	}
	UActorComponent* Component = Actor->FindComponentByClass(Class);
	if (!Component)
	{
		Actor->Modify();
		const FName Name = MakeUniqueObjectName(Actor, Class, FName(*ObjectTools::SanitizeObjectName(C.Name.IsEmpty() ? Class->GetName() : C.Name)));
		Component = NewObject<UActorComponent>(Actor, Class, Name, RF_Transactional);
		if (USceneComponent* Scene = Cast<USceneComponent>(Component))
		{
			Scene->SetupAttachment(Actor->GetRootComponent());
		}
		Actor->AddInstanceComponent(Component);
		Component->OnComponentCreated();
		Component->RegisterComponent();
	}
	SetProperties(Component, C, OutWarnings);
}
} // namespace

void LilaLink::RegisterComponentExporter(const TSharedRef<ILilaComponentExporter>& Exporter)
{
	Exporters().Add(Exporter->GetName(), Exporter);
}

void LilaLink::UnregisterComponentExporter(FName Name)
{
	Exporters().Remove(Name);
}

TArray<FString> LilaLink::ComponentExporterNames()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::ComponentExporterNames);

	TArray<FString> Out;
	for (const TPair<FName, TSharedRef<ILilaComponentExporter>>& Pair : Exporters())
	{
		Out.Add(Pair.Key.ToString());
	}
	if (IPythonScriptPlugin* Py = Python())
	{
		// Guarded by find_spec so a project without the plugin's Content/Python on its path logs nothing.
		FPythonCommandEx Command;
		Command.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
		Command.Command = FString::Printf(TEXT("__import__('%s').names_text() if __import__('importlib.util').util.find_spec('%s') else ''"), PythonModule, PythonModule);
		if (Py->ExecPythonCommandEx(Command))
		{
			FString Text = Command.CommandResult.TrimStartAndEnd();
			if (Text.Len() >= 2 && (Text[0] == TEXT('\'') ||Text[0] == TEXT('"')) && Text[Text.Len() - 1] == Text[0])
			{
				Text = Text.Mid(1, Text.Len() - 2);
			}
			TArray<FString> Names;
			Text.ParseIntoArray(Names, TEXT("|"), true);
			for (const FString& Name : Names)
			{
				Out.AddUnique(Name);
			}
		}
	}
	return Out;
}

FString LilaLink::ComponentValueText(const TSharedPtr<FJsonValue>& V)
{
	return ValueText(V);
}

bool LilaLink::ParseComponents(const TSharedPtr<FJsonObject>& Owner, TArray<FLilaComponent>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (!Owner.IsValid() || !Owner->TryGetArrayField(TEXT("components"), List))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *List)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!V.IsValid() || !V->TryGetObject(Object))
		{
			continue;
		}
		FLilaComponent C;
		(*Object)->TryGetStringField(TEXT("name"), C.Name);
		C.Mode = TEXT("assetUserData");
		(*Object)->TryGetStringField(TEXT("mode"), C.Mode);
		(*Object)->TryGetStringField(TEXT("class"), C.Class);
		const TSharedPtr<FJsonObject>* Attrs = nullptr;
		if ((*Object)->TryGetObjectField(TEXT("attrs"), Attrs) && Attrs->IsValid())
		{
			C.Attrs = Attrs->ToSharedRef();
		}
		Out.Add(MoveTemp(C));
	}
	return true;
}

void LilaLink::ApplyComponentsToAsset(UObject* Asset, const TArray<FLilaComponent>& Components, const FString& Folder, TArray<FString>& OutWarnings,
	TArray<FString>& OutAssets)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::ApplyComponentsToAsset);

	TMap<FString, FLilaComponentValues> Data;
	for (const FLilaComponent& C : Components)
	{
		if (C.Mode == TEXT("none"))
		{
			continue;
		}
		if (C.Mode == TEXT("custom"))
		{
			RunExporter(C, nullptr, Asset, OutWarnings);
			continue;
		}
		FLilaComponentValues Values = ValuesOf(C);
		if (C.Mode == TEXT("dataAsset"))
		{
			Values.DataAsset = MakeDataAsset(C, Folder, Asset->GetName(), OutWarnings, OutAssets);
		}
		Data.Add(C.Name, MoveTemp(Values));
	}
	WriteUserData(Asset, MoveTemp(Data), {});
}

void LilaLink::ApplyComponentsToActor(AActor* Actor, const TArray<FLilaComponent>& Components, const FString& Folder, TArray<FString>& OutWarnings,
	TArray<FString>& OutAssets)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::ApplyComponentsToActor);

	USceneComponent* Root = Actor->GetRootComponent();
	TMap<FString, FLilaComponentValues> Data;
	TArray<FName> Tags;
	for (const FLilaComponent& C : Components)
	{
		if (C.Mode == TEXT("none"))
		{
			continue;
		}
		if (C.Mode == TEXT("actorTags"))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Attr : C.Attrs->Values)
			{
				const FString Key = C.Name.IsEmpty() ? Attr.Key : C.Name + TEXT(".") + Attr.Key;
				Tags.Add(FName(*(Key + TEXT("=") + ValueText(Attr.Value))));
			}
		}
		else if (C.Mode == TEXT("component"))
		{
			ApplyActorComponent(Actor, C, OutWarnings);
		}
		else if (C.Mode == TEXT("custom"))
		{
			RunExporter(C, Actor, nullptr, OutWarnings);
		}
		else
		{
			if (C.Mode != TEXT("assetUserData") && C.Mode != TEXT("dataAsset"))
			{
				OutWarnings.Add(FString::Printf(TEXT("%s: unknown export mode %s; kept as asset user data"), *C.Name, *C.Mode));
			}
			FLilaComponentValues Values = ValuesOf(C);
			if (C.Mode == TEXT("dataAsset"))
			{
				Values.DataAsset = MakeDataAsset(C, Folder, Actor->GetActorLabel(), OutWarnings, OutAssets);
			}
			Data.Add(C.Name, MoveTemp(Values));
		}
	}

	const ULilaComponentUserData* Previous = Root ? ULilaComponentUserData::FindOn(Root) : nullptr;
	TArray<FName> Old = Previous ? Previous->ActorTags : TArray<FName>();
	if (Old != Tags)
	{
		Actor->Modify();
		Actor->Tags.RemoveAll([&Old](const FName& T) { return Old.Contains(T); });
		for (const FName& T : Tags)
		{
			Actor->Tags.AddUnique(T);
		}
	}
	if (Root)
	{
		WriteUserData(Root, MoveTemp(Data), MoveTemp(Tags));
	}
	else if (!Data.IsEmpty())
	{
		OutWarnings.Add(FString::Printf(TEXT("%s has no root component to hold its asset user data"), *Actor->GetActorLabel()));
	}
}
