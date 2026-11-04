#include "RigUnit_LilaExpression.h"

#include "LilaExpressionVm.h"
#include "Dom/JsonObject.h"
#include "Misc/ScopeLock.h"
#include "Rigs/RigHierarchy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Units/RigUnitContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RigUnit_LilaExpression)

namespace
{
using namespace lila_expr;

struct FBinding
{
	enum class EKind : uint8
	{
		Bone,
		Curve,
		Control,
	};
	EKind Kind = EKind::Curve;
	FName Name;
	int32 Channel = 0;
	channels::Joint Joint;
};

struct FParsed
{
	FString Program;
	FString Bindings;
	lila_expr::Program Prog;
	channels::Basis Basis;
	TArray<FBinding> Inputs;
	FBinding Target;
	bool bClampMin = false;
	bool bClampMax = false;
	double Min = 0.0;
	double Max = 0.0;
	FString Error;
};

void ReadM3(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, channels::M3& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Obj->TryGetArrayField(Field, Values) && Values->Num() == 9)
	{
		for (int32 I = 0; I < 9; ++I)
		{
			Out.m[I / 3][I % 3] = (*Values)[I]->AsNumber();
		}
	}
}

void ReadDoubles(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double* Out, int32 Count)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Obj->TryGetArrayField(Field, Values) && Values->Num() == Count)
	{
		for (int32 I = 0; I < Count; ++I)
		{
			Out[I] = (*Values)[I]->AsNumber();
		}
	}
}

bool ReadBinding(const TSharedPtr<FJsonObject>& Obj, FBinding& Out, FString& OutError)
{
	if (!Obj.IsValid())
	{
		OutError = TEXT("a binding isn't an object");
		return false;
	}
	const FString Kind = Obj->GetStringField(TEXT("kind"));
	Out.Name = FName(*Obj->GetStringField(TEXT("name")));
	if (Kind == TEXT("bone"))
	{
		Out.Kind = FBinding::EKind::Bone;
		Out.Channel = FMath::Clamp(int32(Obj->GetNumberField(TEXT("channel"))), 0, 8);
		Out.Joint.order = int32(Obj->GetNumberField(TEXT("order")));
		ReadM3(Obj, TEXT("orient"), Out.Joint.orient);
		ReadM3(Obj, TEXT("axis"), Out.Joint.axis);
		ReadM3(Obj, TEXT("parentRotation"), Out.Joint.parentRotation);
		ReadDoubles(Obj, TEXT("parentTranslation"), Out.Joint.parentTranslation, 3);
		ReadDoubles(Obj, TEXT("rest"), Out.Joint.rest, 9);
	}
	else if (Kind == TEXT("curve"))
	{
		Out.Kind = FBinding::EKind::Curve;
	}
	else if (Kind == TEXT("control"))
	{
		Out.Kind = FBinding::EKind::Control;
	}
	else
	{
		OutError = FString::Printf(TEXT("unknown binding kind '%s'"), *Kind);
		return false;
	}
	return true;
}

TSharedPtr<FParsed> Parse(const FString& Program, const FString& Bindings)
{
	TSharedPtr<FParsed> Out = MakeShared<FParsed>();
	Out->Program = Program;
	Out->Bindings = Bindings;
	std::string Why;
	if (!lila_expr::parse(TCHAR_TO_UTF8(*Program), Out->Prog, &Why))
	{
		Out->Error = FString::Printf(TEXT("program: %s"), UTF8_TO_TCHAR(Why.c_str()));
		return Out;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Bindings), Root) || !Root.IsValid())
	{
		Out->Error = TEXT("Bindings isn't JSON");
		return Out;
	}
	ReadM3(Root, TEXT("basis"), Out->Basis.k);
	Root->TryGetNumberField(TEXT("unitScale"), Out->Basis.unitScale);
	Out->bClampMin = Root->TryGetNumberField(TEXT("min"), Out->Min);
	Out->bClampMax = Root->TryGetNumberField(TEXT("max"), Out->Max);
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (Root->TryGetArrayField(TEXT("inputs"), Inputs))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Inputs)
		{
			FBinding& B = Out->Inputs.AddDefaulted_GetRef();
			if (!ReadBinding(Value->AsObject(), B, Out->Error))
			{
				return Out;
			}
		}
	}
	if (Out->Inputs.Num() != Out->Prog.inputs)
	{
		Out->Error = FString::Printf(TEXT("the program reads %d inputs, Bindings gives %d"), Out->Prog.inputs, Out->Inputs.Num());
		return Out;
	}
	const TSharedPtr<FJsonObject>* Target = nullptr;
	if (!Root->TryGetObjectField(TEXT("target"), Target))
	{
		Out->Error = TEXT("Bindings has no target");
		return Out;
	}
	ReadBinding(*Target, Out->Target, Out->Error);
	return Out;
}

/** Parsed programs by their text: Execute runs every frame, parsing only when the pins change. */
TSharedPtr<FParsed> Cached(const FString& Program, const FString& Bindings)
{
	static FCriticalSection Lock;
	static TMap<uint32, TSharedPtr<FParsed>> Cache;
	const uint32 Key = HashCombine(GetTypeHash(Program), GetTypeHash(Bindings));
	FScopeLock Guard(&Lock);
	if (TSharedPtr<FParsed>* Found = Cache.Find(Key); Found && (*Found)->Program == Program && (*Found)->Bindings == Bindings)
	{
		return *Found;
	}
	TSharedPtr<FParsed> Parsed = Parse(Program, Bindings);
	Cache.Add(Key, Parsed);
	return Parsed;
}

channels::BoneTransform ToBone(const FTransform& T)
{
	channels::BoneTransform B;
	const FQuat Q = T.GetRotation();
	B.rotation[0] = Q.X;
	B.rotation[1] = Q.Y;
	B.rotation[2] = Q.Z;
	B.rotation[3] = Q.W;
	const FVector L = T.GetLocation(), S = T.GetScale3D();
	for (int32 I = 0; I < 3; ++I)
	{
		B.translation[I] = L[I];
		B.scale[I] = S[I];
	}
	return B;
}

FTransform FromBone(const channels::BoneTransform& B)
{
	return FTransform(FQuat(B.rotation[0], B.rotation[1], B.rotation[2], B.rotation[3]).GetNormalized(),
	                  FVector(B.translation[0], B.translation[1], B.translation[2]), FVector(B.scale[0], B.scale[1], B.scale[2]));
}

double ReadInput(URigHierarchy* Hierarchy, const FParsed& P, const FBinding& B)
{
	switch (B.Kind)
	{
	case FBinding::EKind::Bone:
	{
		const FRigElementKey Key(B.Name, ERigElementType::Bone);
		if (!Hierarchy->Contains(Key))
		{
			return B.Joint.rest[B.Channel];
		}
		double Channels[9];
		channels::read(ToBone(Hierarchy->GetLocalTransform(Key)), B.Joint, P.Basis, Channels);
		return Channels[B.Channel];
	}
	case FBinding::EKind::Curve:
	{
		const FRigElementKey Key(B.Name, ERigElementType::Curve);
		return Hierarchy->Contains(Key) ? double(Hierarchy->GetCurveValue(Key)) : 0.0;
	}
	case FBinding::EKind::Control:
	{
		const FRigElementKey Key(B.Name, ERigElementType::Control);
		return Hierarchy->Contains(Key) ? double(Hierarchy->GetControlValue(Key).Get<float>()) : 0.0;
	}
	}
	return 0.0;
}
} // namespace

FRigUnit_LilaExpression_Execute()
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_RIGUNIT()
	URigHierarchy* Hierarchy = ExecuteContext.Hierarchy;
	if (!Hierarchy)
	{
		return;
	}
	const TSharedPtr<FParsed> P = Cached(Program, Bindings);
	if (!P->Error.IsEmpty())
	{
		UE_CONTROLRIG_RIGUNIT_REPORT_ERROR(TEXT("%s"), *P->Error);
		return;
	}
	TArray<double, TInlineAllocator<8>> Values;
	for (const FBinding& B : P->Inputs)
	{
		Values.Add(ReadInput(Hierarchy, *P, B));
	}
	Result = lila_expr::evaluate(P->Prog, Values.GetData(), ExecuteContext.GetAbsoluteTime<double>());
	if (P->bClampMin)
	{
		Result = FMath::Max(Result, P->Min);
	}
	if (P->bClampMax)
	{
		Result = FMath::Min(Result, P->Max);
	}

	const FBinding& T = P->Target;
	switch (T.Kind)
	{
	case FBinding::EKind::Bone:
	{
		const FRigElementKey Key(T.Name, ERigElementType::Bone);
		if (!Hierarchy->Contains(Key))
		{
			UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("no bone %s"), *T.Name.ToString());
			return;
		}
		double Channels[9];
		channels::read(ToBone(Hierarchy->GetLocalTransform(Key)), T.Joint, P->Basis, Channels);
		Channels[T.Channel] = Result;
		Hierarchy->SetLocalTransform(Key, FromBone(channels::write(Channels, T.Joint, P->Basis)), false, true);
		break;
	}
	case FBinding::EKind::Curve:
	{
		const FRigElementKey Key(T.Name, ERigElementType::Curve);
		if (Hierarchy->Contains(Key))
		{
			Hierarchy->SetCurveValue(Key, float(Result));
		}
		break;
	}
	case FBinding::EKind::Control:
	{
		const FRigElementKey Key(T.Name, ERigElementType::Control);
		if (Hierarchy->Contains(Key))
		{
			Hierarchy->SetControlValue(Key, FRigControlValue::Make<float>(float(Result)));
		}
		break;
	}
	}
}
