#include "RigUnit_LilaStrings.h"

#include "LilaStringsSolver.h"

#include "ControlRig.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/RigUnitContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RigUnit_LilaStrings)

namespace
{
using namespace LilaStrings;

/** The rule web by bone name, as Lila's stringsRuleWeb writes it: the bones as Strings solves them, parents first. */
struct FRuleWeb
{
	FString Source;
	FName Hips;
	TArray<FName> Bones;
	TArray<FName> Parents;
	TArray<bool> Solve;
	TArray<double> Stiffness;
	TArray<FLimit> Limits;
	TArray<FName> Contacts;
	TArray<TArray<FName>> Chains;
	TArray<FName> Isolate;
	TArray<TPair<FName, EMode>> Modes;
	double LengthScale = 0.0;
	FString Error;
};

/** Lila's mode names (strings::modeName) to the solver's modes. */
TOptional<EMode> ModeNamed(const FString& Name)
{
	static const TPair<const TCHAR*, EMode> Names[] = {
		{TEXT("default"), EMode::Default},
		{TEXT("free"), EMode::Free},
		{TEXT("forward"), EMode::Forward},
		{TEXT("forwardAll"), EMode::ForwardAll},
		{TEXT("backward"), EMode::Backward},
		{TEXT("backwardAll"), EMode::BackwardAll},
		{TEXT("bidirectional"), EMode::Bidirectional},
		{TEXT("all"), EMode::All},
		{TEXT("ik"), EMode::Ik},
		{TEXT("fullBody"), EMode::FullBody},
	};
	for (const TPair<const TCHAR*, EMode>& N : Names)
	{
		if (Name.Equals(N.Key, ESearchCase::IgnoreCase))
		{
			return N.Value;
		}
	}
	return {};
}

EMode ModeOf(const ELilaStringsMode Mode)
{
	switch (Mode)
	{
	case ELilaStringsMode::Default: return EMode::Default;
	case ELilaStringsMode::Free: return EMode::Free;
	case ELilaStringsMode::Forward: return EMode::Forward;
	case ELilaStringsMode::ForwardAll: return EMode::ForwardAll;
	case ELilaStringsMode::Backward: return EMode::Backward;
	case ELilaStringsMode::BackwardAll: return EMode::BackwardAll;
	case ELilaStringsMode::Bidirectional: return EMode::Bidirectional;
	case ELilaStringsMode::All: return EMode::All;
	case ELilaStringsMode::IK: return EMode::Ik;
	case ELilaStringsMode::FullBody: return EMode::FullBody;
	}
	return EMode::FullBody;
}

TArray<FName> ReadNames(const TArray<TSharedPtr<FJsonValue>>& Values)
{
	TArray<FName> Out;
	for (const TSharedPtr<FJsonValue>& V : Values)
	{
		Out.Add(FName(*V->AsString()));
	}
	return Out;
}

FVector ReadVector3(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& Fallback)
{
	const TArray<TSharedPtr<FJsonValue>>* V = nullptr;
	return Obj->TryGetArrayField(Field, V) && V->Num() == 3 ? FVector((*V)[0]->AsNumber(), (*V)[1]->AsNumber(), (*V)[2]->AsNumber()) : Fallback;
}

/** A bone's limit as stringsRuleWeb writes it, already in Unreal's local-rotation space. */
FLimit ReadLimit(const TSharedPtr<FJsonObject>& Obj)
{
	FLimit L;
	const FString Kind = Obj->GetStringField(TEXT("kind"));
	L.Kind = Kind == TEXT("hinge") ? FLimit::EKind::Hinge : Kind == TEXT("cone") ? FLimit::EKind::Cone : FLimit::EKind::None;
	L.Axis = ReadVector3(Obj, TEXT("axis"), FVector::ZAxisVector);
	L.BoneDir = ReadVector3(Obj, TEXT("bone"), FVector::XAxisVector);
	Obj->TryGetNumberField(TEXT("min"), L.Min);
	Obj->TryGetNumberField(TEXT("max"), L.Max);
	Obj->TryGetNumberField(TEXT("cone"), L.Cone);
	const TArray<TSharedPtr<FJsonValue>>* Post = nullptr;
	if (Obj->TryGetArrayField(TEXT("post"), Post) && Post->Num() == 4)
	{
		L.Post = FQuat((*Post)[0]->AsNumber(), (*Post)[1]->AsNumber(), (*Post)[2]->AsNumber(), (*Post)[3]->AsNumber()).GetNormalized();
	}
	return L;
}

TSharedPtr<FRuleWeb> Parse(const FString& Text)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::Parse);

	TSharedPtr<FRuleWeb> Web = MakeShared<FRuleWeb>();
	Web->Source = Text;
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		Web->Error = TEXT("the rule web isn't JSON; make it with Lila's stringsRuleWeb");
		return Web;
	}
	Web->Hips = FName(*Root->GetStringField(TEXT("hips")));
	const TSharedPtr<FJsonObject>* Stiff = nullptr;
	Root->TryGetObjectField(TEXT("stiffness"), Stiff);
	const TSharedPtr<FJsonObject>* LimitsOf = nullptr;
	Root->TryGetObjectField(TEXT("limits"), LimitsOf);
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Root->TryGetArrayField(TEXT("bones"), Values))
	{
		for (const TSharedPtr<FJsonValue>& B : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = B->AsObject();
			const FString Name = Obj->GetStringField(TEXT("name"));
			const FString ParentName = Obj->GetStringField(TEXT("parent"));
			bool bSolve = true;
			Obj->TryGetBoolField(TEXT("solve"), bSolve);
			double Stiffness = 0.0;
			if (Stiff)
			{
				(*Stiff)->TryGetNumberField(Name, Stiffness);
			}
			Web->Bones.Add(FName(*Name));
			Web->Parents.Add(ParentName.IsEmpty() ? NAME_None : FName(*ParentName));
			Web->Solve.Add(bSolve);
			Web->Stiffness.Add(Stiffness);
			const TSharedPtr<FJsonObject>* Limit = nullptr;
			Web->Limits.Add(LimitsOf && (*LimitsOf)->TryGetObjectField(Name, Limit) ? ReadLimit(*Limit) : FLimit());
		}
	}
	if (Root->TryGetArrayField(TEXT("contacts"), Values))
	{
		Web->Contacts = ReadNames(*Values);
	}
	if (Root->TryGetArrayField(TEXT("chains"), Values))
	{
		for (const TSharedPtr<FJsonValue>& C : *Values)
		{
			Web->Chains.Add(ReadNames(C->AsArray()));
		}
	}
	if (Root->TryGetArrayField(TEXT("isolate"), Values))
	{
		Web->Isolate = ReadNames(*Values);
	}
	const TSharedPtr<FJsonObject>* ModesOf = nullptr;
	if (Root->TryGetObjectField(TEXT("modes"), ModesOf))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& M : (*ModesOf)->Values)
		{
			if (const TOptional<EMode> Mode = ModeNamed(M.Value->AsString()))
			{
				Web->Modes.Emplace(FName(*M.Key), *Mode);
			}
		}
	}
	Root->TryGetNumberField(TEXT("lengthScale"), Web->LengthScale);
	return Web;
}

/** Parsed once per distinct web, shared by every unit and instance using it. */
TSharedPtr<FRuleWeb> Cached(const FString& Text)
{
	static FCriticalSection Lock;
	static TMap<uint32, TSharedPtr<FRuleWeb>> Cache;
	const uint32 Key = GetTypeHash(Text);
	FScopeLock Guard(&Lock);
	if (TSharedPtr<FRuleWeb>* Found = Cache.Find(Key); Found && (*Found)->Source == Text)
	{
		return *Found;
	}
	TSharedPtr<FRuleWeb> Web = Parse(Text);
	Cache.Add(Key, Web);
	return Web;
}

FRigElementKey BoneKey(const FName Name) { return FRigElementKey(Name, ERigElementType::Bone); }

/** The skeleton as the web's problem, from the hierarchy's current pose; false with Missing set when a bone isn't there. */
bool ProblemOf(URigHierarchy* H, const FRuleWeb& W, FProblem& Out, FRules& Rules, FName& Missing)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::ProblemOf);

	TMap<FName, int32> Index;
	FVector Lo(TNumericLimits<double>::Max()), Hi(-TNumericLimits<double>::Max());
	for (int32 I = 0; I < W.Bones.Num(); ++I)
	{
		const FRigElementKey Key = BoneKey(W.Bones[I]);
		if (!H->Contains(Key))
		{
			Missing = W.Bones[I];
			return false;
		}
		const FTransform Local = H->GetLocalTransform(Key);
		const FTransform Global = H->GetGlobalTransform(Key);
		FBone& B = Out.Bones.AddDefaulted_GetRef();
		const int32* Parent = W.Parents[I].IsNone() ? nullptr : Index.Find(W.Parents[I]);
		B.Parent = Parent ? *Parent : INDEX_NONE;
		if (B.Parent == INDEX_NONE)
		{
			// Global is Local then the parent's global, so the parent outside the solve is Local^-1 then Global.
			B.ParentWorld = Local.ToMatrixWithScale().Inverse() * Global.ToMatrixWithScale();
		}
		B.Before = FTranslationMatrix(Local.GetTranslation());
		B.After = FScaleMatrix(Local.GetScale3D());
		B.Rotation = B.Reference = Local.GetRotation().GetNormalized();
		B.bSolve = W.Solve[I];
		B.Stiffness = W.Stiffness[I];
		B.Limit = W.Limits[I];
		Index.Add(W.Bones[I], I);
		Lo = Lo.ComponentMin(Global.GetLocation());
		Hi = Hi.ComponentMax(Global.GetLocation());
	}
	Out.LengthScale = W.LengthScale > 0.0 ? W.LengthScale : FMath::Max((Hi - Lo).Size() * 0.2, 1.0);
	for (const TArray<FName>& Names : W.Chains)
	{
		TArray<int32> Chain;
		for (const FName& N : Names)
		{
			if (const int32* I = Index.Find(N))
			{
				Chain.Add(*I);
			}
		}
		if (Chain.Num() >= 2)
		{
			Rules.Chains.Add(MoveTemp(Chain));
		}
	}
	for (const TPair<FName, EMode>& M : W.Modes)
	{
		if (const int32* I = Index.Find(M.Key))
		{
			Rules.Defaults.Emplace(*I, M.Value);
		}
	}
	return true;
}

/** Sets a bone's local transform even when it moved less than the hierarchy's 1e-4 change test, as a limb held at a limit can. */
void SetLocalForced(URigHierarchy* H, const FRigElementKey& Key, const FTransform& Local)
{
	if (FRigTransformElement* E = H->Find<FRigTransformElement>(Key))
	{
		H->SetTransform(E, Local, ERigTransformType::CurrentLocal, true, false, true);
	}
}

/** Writes the pose into the hierarchy, parents first, as each bone's local transform. */
void ApplyPose(URigHierarchy* H, const FRuleWeb& W, const FProblem& P, const FPose& S)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::ApplyPose);

	for (int32 I = 0; I < P.Bones.Num(); ++I)
	{
		const FBone& B = P.Bones[I];
		const FVector Lengthen = (S.Stretch[I] - 1.0) * B.Before.GetOrigin();
		const FMatrix Local = Mul(Mul(Mul(FTranslationMatrix(S.Offset[I] + Lengthen), B.Before), FQuatRotationMatrix(S.Rotation[I])), B.After);
		SetLocalForced(H, BoneKey(W.Bones[I]), FTransform(Local));
	}
}

/** Every bone of the web as it is, to blend toward by Alpha after the solve. */
struct FPoseBlend
{
	TArray<TPair<FName, FTransform>> Before;

	void Capture(URigHierarchy* H, const FRuleWeb& W)
	{
		for (const FName& Bone : W.Bones)
		{
			Before.Emplace(Bone, H->GetLocalTransform(BoneKey(Bone)));
		}
	}

	void Apply(URigHierarchy* H, const float Alpha) const
	{
		if (Alpha >= 1.0f)
		{
			return;
		}
		for (const TPair<FName, FTransform>& B : Before)
		{
			FTransform Blended;
			Blended.Blend(B.Value, H->GetLocalTransform(BoneKey(B.Key)), FMath::Clamp(Alpha, 0.0f, 1.0f));
			SetLocalForced(H, BoneKey(B.Key), Blended);
		}
	}
};

/** What a unit asks for, whichever mode. */
struct FAsk
{
	const FString& RuleWeb;
	FName Bone;
	FName Control;
	const FTransform& Target;
	bool bHoldContacts = true;
	float Alpha = 1.0f;
	bool bRotate = true;
	int32 ChainLength = 0;
	bool bIsolate = false;
	double MaxStretch = 1.0;
	/** What a bone the mode can't move (above the hips in Backward or IK) poses in instead. */
	EMode Fallback = EMode::Forward;
};

/** The control's animation channel named Name, when it has one. */
const FRigControlElement* ChannelOf(const URigHierarchy* H, const FRigElementKey& Control, const FName Name)
{
	const FRigControlElement* Element = H->Find<FRigControlElement>(Control);
	if (!Element)
	{
		return nullptr;
	}
	for (const FRigBaseElement* Child : H->GetChildren(Element))
	{
		if (const FRigControlElement* Channel = Cast<FRigControlElement>(Child); Channel && Channel->IsAnimationChannel() && Channel->GetDisplayName().IsEqual(Name))
		{
			return Channel;
		}
	}
	return nullptr;
}

/** Mode and Isolate Rotation from the control's channels where it has them, else as the unit's pins give them. */
void ModeFromChannels(const URigHierarchy* H, const FRigElementKey& Control, EMode& Mode, bool& bIsolate)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::ModeFromChannels);

	if (const FRigControlElement* M = ChannelOf(H, Control, TEXT("Mode")))
	{
		const int32 Index = H->GetControlValue(M->GetKey()).Get<int32>();
		const UEnum* Enum = StaticEnum<ELilaStringsMode>();
		// NumEnums counts the generated _MAX entry.
		if (Index >= 0 && Index < Enum->NumEnums() - 1)
		{
			Mode = ModeOf(ELilaStringsMode(Enum->GetValueByIndex(Index)));
		}
	}
	if (const FRigControlElement* I = ChannelOf(H, Control, TEXT("Isolate Rotation")))
	{
		bIsolate = H->GetControlValue(I->GetKey()).Get<bool>();
	}
}

/** Poses Ask.Bone in Mode as Lila's stringsPose does, from the hierarchy's pose. False (with a warning) when it can't. */
bool Run(URigHierarchy* H, EMode Mode, const FAsk& Ask, float& OutError, const TFunctionRef<void(const FString&)> Warn)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::Run);

	if (!H || Ask.Alpha <= 0.0f)
	{
		return false;
	}
	const TSharedPtr<FRuleWeb> Web = Cached(Ask.RuleWeb);
	const FRuleWeb& W = *Web;
	if (!W.Error.IsEmpty())
	{
		Warn(W.Error);
		return false;
	}
	FProblem P;
	FRules Rules;
	FName Missing;
	if (!ProblemOf(H, W, P, Rules, Missing))
	{
		Warn(FString::Printf(TEXT("No bone named %s, which the rule web poses"), *Missing.ToString()));
		return false;
	}
	const int32 Grabbed = W.Bones.IndexOfByKey(Ask.Bone);
	if (Grabbed == INDEX_NONE)
	{
		Warn(FString::Printf(TEXT("%s isn't a bone the rule web poses"), *Ask.Bone.ToString()));
		return false;
	}
	const FRigElementKey ControlKey(Ask.Control, ERigElementType::Control);
	const bool bControl = !Ask.Control.IsNone() && H->Contains(ControlKey);
	bool bIsolate = Ask.bIsolate;
	if (bControl)
	{
		ModeFromChannels(H, ControlKey, Mode, bIsolate);
	}
	const FTransform Goal = bControl ? H->GetGlobalTransform(ControlKey) : Ask.Target;
	// A control left where it started asks for nothing: posing toward it would undo what the dragged controls' units did.
	if (bControl && FRigComputedTransform::Equals(Goal, H->GetGlobalTransform(ControlKey, true), 1e-9f))
	{
		OutError = 0.0f;
		return true;
	}
	const FTransform Now = H->GetGlobalTransform(BoneKey(Ask.Bone));

	FRequest R;
	R.Mode = Mode;
	R.Grabbed = Grabbed;
	R.Hips = W.Bones.IndexOfByKey(W.Hips);
	R.ChainLength = Ask.ChainLength;
	R.MaxStretch = FMath::Max(1.0, Ask.MaxStretch);
	R.Rules = &Rules;
	if (bIsolate)
	{
		for (const FName& Name : W.Isolate)
		{
			if (const int32 B = W.Bones.IndexOfByKey(Name); B != INDEX_NONE)
			{
				R.Isolate.Add(B);
			}
		}
	}
	// Control values are stored in single precision: a control dragged only one way sits a few 1e-6 cm and 1e-7 rad off its
	// bone the other way, which must not read as a move or turn (a turn pins the rotation; a move in Forward replaces the turn).
	const double MoveTolerance = bControl ? 1e-4 : 1e-6;
	const double TurnTolerance = bControl ? 1e-6 : 1e-9;
	if (const FVector Move = Goal.GetLocation() - Now.GetLocation(); !Move.IsNearlyZero(MoveTolerance))
	{
		R.Move = Move;
	}
	// IK and Full Body take the target's rotation when asked to; the other modes always do.
	const FQuat Turn = (Goal.GetRotation() * Now.GetRotation().Inverse()).GetNormalized();
	if ((Ask.bRotate || (Mode != EMode::Ik && Mode != EMode::FullBody)) && !Turn.Equals(FQuat::Identity, TurnTolerance))
	{
		R.Turn = Turn;
	}
	if (Ask.bHoldContacts)
	{
		const TArray<FMatrix> Start = Worlds(P, StartPose(P));
		for (const FName& C : W.Contacts)
		{
			const int32 Bone = W.Bones.IndexOfByKey(C);
			if (Bone != INDEX_NONE && Bone != Grabbed && !R.Holds.ContainsByPredicate([&](const FHold& Hold) { return Hold.Bone == Bone; }))
			{
				R.Holds.Add({Bone, Start[Bone], true});
			}
		}
	}
	// Full Body turning with nothing held is a plain turn: no solve has anything to keep.
	if (Mode == EMode::FullBody && !R.Move && R.Holds.IsEmpty())
	{
		R.Mode = EMode::ForwardAll;
	}
	FPoseBlend Blend;
	Blend.Capture(H, W);
	FResult Res = SolveRequest(P, R);
	if (!Res.bHandled)
	{
		// A bone above the hips has no body to swing or limb to bend.
		R.Mode = Ask.Fallback;
		Res = SolveRequest(P, R);
	}
	ApplyPose(H, W, P, Res.Pose);
	Blend.Apply(H, Ask.Alpha);
	OutError = float(Res.Error);
	return true;
}
} // namespace

FRigUnit_LilaStrings_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStrings::Execute);

	FAsk Ask{RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, bRotate, ChainLength};
	Ask.bIsolate = bIsolateRotation;
	Ask.MaxStretch = MaxStretch;
	Run(ExecuteContext.Hierarchy, ModeOf(Mode), Ask, Error, [&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

namespace
{
/** The deprecated units pose as they always did: plain FK where their mode can't move the bone, no isolation, no stretch. */
FAsk LegacyAsk(FAsk Ask)
{
	Ask.Fallback = EMode::ForwardAll;
	return Ask;
}

FRigUnit_LilaStrings Upgraded(const FRigUnit_LilaStringsBase& U, const ELilaStringsMode Mode, const bool bRotate, const int32 ChainLength)
{
	FRigUnit_LilaStrings New;
	New.RuleWeb = U.RuleWeb;
	New.Bone = U.Bone;
	New.Control = U.Control;
	New.Target = U.Target;
	New.bHoldContacts = U.bHoldContacts;
	New.Alpha = U.Alpha;
	New.Mode = Mode;
	New.bRotate = bRotate;
	New.ChainLength = ChainLength;
	return New;
}
} // namespace

FRigUnit_LilaStringsForward_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsForward::Execute);

	Run(ExecuteContext.Hierarchy, EMode::ForwardAll, LegacyAsk({RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, true, 0}), Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigVMStructUpgradeInfo FRigUnit_LilaStringsForward::GetUpgradeInfo() const
{
	return FRigVMStructUpgradeInfo(*this, Upgraded(*this, ELilaStringsMode::ForwardAll, true, 0));
}

FRigUnit_LilaStringsBackward_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsBackward::Execute);

	Run(ExecuteContext.Hierarchy, EMode::Backward, LegacyAsk({RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, true, 0}), Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigVMStructUpgradeInfo FRigUnit_LilaStringsBackward::GetUpgradeInfo() const
{
	return FRigVMStructUpgradeInfo(*this, Upgraded(*this, ELilaStringsMode::Backward, true, 0));
}

FRigUnit_LilaStringsIK_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsIK::Execute);

	Run(ExecuteContext.Hierarchy, EMode::Ik, LegacyAsk({RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, bRotate, ChainLength}), Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigVMStructUpgradeInfo FRigUnit_LilaStringsIK::GetUpgradeInfo() const
{
	return FRigVMStructUpgradeInfo(*this, Upgraded(*this, ELilaStringsMode::IK, bRotate, ChainLength));
}

FRigUnit_LilaStringsFullBody_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsFullBody::Execute);

	Run(ExecuteContext.Hierarchy, EMode::FullBody, LegacyAsk({RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, bRotate, 0}), Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigVMStructUpgradeInfo FRigUnit_LilaStringsFullBody::GetUpgradeInfo() const
{
	return FRigVMStructUpgradeInfo(*this, Upgraded(*this, ELilaStringsMode::FullBody, bRotate, 0));
}

TMap<FName, FTransform> ULilaStringsLibrary::SolveOnMesh(USkeletalMesh* Mesh, const FString& RuleWeb, const FString& Mode, const FName Bone, const FTransform& Target,
														 const bool bRotate, const bool bHoldContacts, const int32 ChainLength, const FVector TurnVector, float& Error,
														 const bool bIsolateRotation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStringsLibrary::SolveOnMesh);

	TMap<FName, FTransform> Out;
	Error = -1.0f;
	if (!Mesh)
	{
		return Out;
	}
	const TOptional<EMode> Found = ModeNamed(Mode);
	if (!Found)
	{
		UE_LOG(LogTemp, Warning, TEXT("Lila Strings: no mode %s (default, free, forward, forwardAll, backward, backwardAll, bidirectional, all, ik, fullBody)"), *Mode);
		return Out;
	}
	URigHierarchy* H = NewObject<URigHierarchy>(GetTransientPackage());
	H->GetController(true)->ImportBones(Mesh->GetRefSkeleton(), NAME_None, true, true, false, false);
	FTransform Goal = Target;
	if (const double Angle = TurnVector.Size(); Angle > 0.0)
	{
		Goal.SetRotation(FQuat(TurnVector / Angle, Angle) * H->GetGlobalTransform(FRigElementKey(Bone, ERigElementType::Bone)).GetRotation());
	}
	FAsk Ask{RuleWeb, Bone, NAME_None, Goal, bHoldContacts, 1.0f, bRotate, ChainLength};
	Ask.bIsolate = bIsolateRotation;
	if (Run(H, *Found, Ask, Error,
			[](const FString& Message) { UE_LOG(LogTemp, Warning, TEXT("Lila Strings: %s"), *Message); }))
	{
		H->ForEach<FRigBoneElement>([&](FRigBoneElement* E) {
			Out.Add(E->GetFName(), H->GetGlobalTransform(E->GetKey()));
			return true;
		});
	}
	return Out;
}

bool ULilaStringsLibrary::PlaceControlAtBone(URigHierarchy* Hierarchy, const FName Control, const FName Bone)
{
	const FRigElementKey ControlKey(Control, ERigElementType::Control);
	if (!Hierarchy || !Hierarchy->Contains(ControlKey) || !Hierarchy->Contains(BoneKey(Bone)))
	{
		return false;
	}
	const FTransform Rest = Hierarchy->GetGlobalTransform(BoneKey(Bone), true);
	Hierarchy->SetGlobalTransform(ControlKey, Rest, true, false);
	Hierarchy->SetGlobalTransform(ControlKey, Rest, false, false);
	return true;
}

bool ULilaStringsLibrary::SetModeEnum(URigHierarchy* Hierarchy, const FRigElementKey Channel)
{
	const FRigControlElement* Control = Hierarchy ? Hierarchy->Find<FRigControlElement>(Channel) : nullptr;
	if (!Control)
	{
		return false;
	}
	FRigControlSettings Settings = Control->Settings;
	Settings.ControlType = ERigControlType::Integer;
	Settings.ControlEnum = StaticEnum<ELilaStringsMode>();
	Hierarchy->SetControlSettings(Channel, Settings, false, true);
	return true;
}

TMap<FName, FTransform> ULilaStringsLibrary::DragRig(UClass* RigClass, const FName Control, const FTransform& Target, const FVector TurnVector)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStringsLibrary::DragRig);

	TMap<FName, FTransform> Out;
	if (!RigClass || !RigClass->IsChildOf(UControlRig::StaticClass()))
	{
		return Out;
	}
	UControlRig* Rig = NewObject<UControlRig>(GetTransientPackage(), RigClass);
	Rig->Initialize(true);
	Rig->Execute(FRigUnit_BeginExecution::EventName);
	URigHierarchy* H = Rig->GetHierarchy();
	if (!H)
	{
		return Out;
	}
	const FRigElementKey Key(Control, ERigElementType::Control);
	FTransform Goal = Target;
	if (const double Angle = TurnVector.Size(); Angle > 0.0)
	{
		Goal.SetRotation(FQuat(TurnVector / Angle, Angle) * H->GetGlobalTransform(Key).GetRotation());
	}
	H->SetGlobalTransform(Key, Goal, false, true);
	Rig->Execute(FRigUnit_BeginExecution::EventName);
	H->ForEach<FRigBoneElement>([&](FRigBoneElement* E) {
		Out.Add(E->GetFName(), H->GetGlobalTransform(E->GetKey()));
		return true;
	});
	return Out;
}
