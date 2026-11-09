#include "RigUnit_LilaStrings.h"

#include "Dom/JsonObject.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rigs/RigHierarchy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Units/RigUnitContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RigUnit_LilaStrings)

namespace
{
/** The rule web by bone name. Names are FNames throughout, so they compare as Unreal compares bone names. */
struct FRuleWeb
{
	FString Source;
	FName Hips;
	TMap<FName, FName> Parent;
	TMap<FName, int32> ChildCount;
	TArray<FName> Contacts;
	TArray<TArray<FName>> Chains;
	TMap<FName, double> Stiffness;
	FString Error;
};

TArray<FName> ReadNames(const TArray<TSharedPtr<FJsonValue>>& Values)
{
	TArray<FName> Out;
	for (const TSharedPtr<FJsonValue>& V : Values)
	{
		Out.Add(FName(*V->AsString()));
	}
	return Out;
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
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Root->TryGetArrayField(TEXT("bones"), Values))
	{
		for (const TSharedPtr<FJsonValue>& B : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = B->AsObject();
			const FName Name(*Obj->GetStringField(TEXT("name")));
			const FString ParentName = Obj->GetStringField(TEXT("parent"));
			const FName ParentBone = ParentName.IsEmpty() ? NAME_None : FName(*ParentName);
			Web->Parent.Add(Name, ParentBone);
			if (!ParentBone.IsNone())
			{
				Web->ChildCount.FindOrAdd(ParentBone)++;
			}
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
	const TSharedPtr<FJsonObject>* Stiff = nullptr;
	if (Root->TryGetObjectField(TEXT("stiffness"), Stiff))
	{
		for (const auto& [Name, Value] : (*Stiff)->Values)
		{
			Web->Stiffness.Add(FName(*Name), Value->AsNumber());
		}
	}
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

bool IsUnder(const FRuleWeb& W, FName Bone, const FName Ancestor)
{
	for (int32 Guard = 0; !Bone.IsNone() && Guard < 512; ++Guard)
	{
		if (Bone == Ancestor)
		{
			return true;
		}
		const FName* P = W.Parent.Find(Bone);
		Bone = P ? *P : NAME_None;
	}
	return false;
}

/**
 * The bones that bend to bring Bone to a target, top first and Bone last: from its declared limb, else up to (not
 * including) a bone that branches or the hips. With Length > 0, that many bones above Bone instead.
 */
TArray<FName> LimbOf(const FRuleWeb& W, const FName Bone, const int32 Length = 0)
{
	TArray<FName> Out{Bone};
	if (Length <= 0)
	{
		for (const TArray<FName>& Chain : W.Chains)
		{
			const int32 At = Chain.IndexOfByKey(Bone);
			if (At > 0)
			{
				return TArray<FName>(Chain.GetData(), At + 1);
			}
		}
	}
	FName Cur = Bone;
	for (int32 Guard = 0; Guard < 64; ++Guard)
	{
		const FName* P = W.Parent.Find(Cur);
		if (!P || P->IsNone() || *P == W.Hips)
		{
			break;
		}
		if (Length > 0 ? Out.Num() > Length : W.ChildCount.FindRef(*P) > 1)
		{
			break;
		}
		Out.Insert(*P, 0);
		Cur = *P;
	}
	return Out;
}

/**
 * Bends a limb (top first, end last) so its end reaches Target: FABRIK on the joint positions, the top fixed, then each
 * bone turned to point at its child's new place. Returns how far the end stays from Target.
 */
double SolveLimb(URigHierarchy* H, const TArray<FName>& Limb, const FVector& Target)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::SolveLimb);

	const int32 N = Limb.Num();
	if (N < 2)
	{
		return (H->GetGlobalTransform(BoneKey(Limb[0])).GetLocation() - Target).Size();
	}
	TArray<FVector> P;
	TArray<double> Len;
	for (const FName& B : Limb)
	{
		P.Add(H->GetGlobalTransform(BoneKey(B)).GetLocation());
	}
	for (int32 I = 0; I + 1 < N; ++I)
	{
		Len.Add((P[I + 1] - P[I]).Size());
	}
	const FVector Root = P[0];
	for (int32 Iteration = 0; Iteration < 16; ++Iteration)
	{
		P[N - 1] = Target;
		for (int32 I = N - 2; I >= 0; --I)
		{
			P[I] = P[I + 1] + (P[I] - P[I + 1]).GetSafeNormal() * Len[I];
		}
		P[0] = Root;
		for (int32 I = 0; I + 1 < N; ++I)
		{
			P[I + 1] = P[I] + (P[I + 1] - P[I]).GetSafeNormal() * Len[I];
		}
		if ((P[N - 1] - Target).SizeSquared() < 1e-6)
		{
			break;
		}
	}
	// Turn each bone so its child lands where FABRIK put it; turning a bone carries everything below.
	for (int32 I = 0; I + 1 < N; ++I)
	{
		const FTransform T = H->GetGlobalTransform(BoneKey(Limb[I]));
		const FVector Child = H->GetGlobalTransform(BoneKey(Limb[I + 1])).GetLocation();
		const FVector From = (Child - T.GetLocation()).GetSafeNormal();
		const FVector To = (P[I + 1] - T.GetLocation()).GetSafeNormal();
		if (From.IsNearlyZero() || To.IsNearlyZero())
		{
			continue;
		}
		FTransform Turned = T;
		Turned.SetRotation(FQuat::FindBetweenNormals(From, To) * T.GetRotation());
		H->SetGlobalTransform(BoneKey(Limb[I]), Turned, false, true);
	}
	return (H->GetGlobalTransform(BoneKey(Limb.Last())).GetLocation() - Target).Size();
}

/** Each contact (but the posed bone and what hangs from it) where it was, its limb bending to reach. */
struct FHolds
{
	TArray<TPair<FName, FTransform>> Saved;

	void Capture(URigHierarchy* H, const FRuleWeb& W, const FName Posed)
	{
		for (const FName& C : W.Contacts)
		{
			if (C != Posed && !IsUnder(W, C, Posed) && H->Contains(BoneKey(C)))
			{
				Saved.Emplace(C, H->GetGlobalTransform(BoneKey(C)));
			}
		}
	}

	double Restore(URigHierarchy* H, const FRuleWeb& W) const
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::RestoreHolds);

		double Worst = 0.0;
		for (const TPair<FName, FTransform>& S : Saved)
		{
			Worst = FMath::Max(Worst, SolveLimb(H, LimbOf(W, S.Key), S.Value.GetLocation()));
			FTransform T = H->GetGlobalTransform(BoneKey(S.Key));
			T.SetRotation(S.Value.GetRotation());
			H->SetGlobalTransform(BoneKey(S.Key), T, false, true);
		}
		return Worst;
	}
};

/** Every bone of the web as it is, to blend toward by Alpha after the solve. */
struct FPoseBlend
{
	TArray<TPair<FName, FTransform>> Before;

	void Capture(URigHierarchy* H, const FRuleWeb& W)
	{
		for (const auto& [Bone, Parent] : W.Parent)
		{
			if (H->Contains(BoneKey(Bone)))
			{
				Before.Emplace(Bone, H->GetLocalTransform(BoneKey(Bone)));
			}
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
			H->SetLocalTransform(BoneKey(B.Key), Blended, false, true);
		}
	}
};

/** What every unit starts from: the web, the hierarchy, the posed bone and where it goes. Null with an error logged when unusable. */
struct FSetup
{
	TSharedPtr<FRuleWeb> Web;
	URigHierarchy* H = nullptr;
	FTransform Goal;
	FTransform Now;
};

bool Prepare(const FRigUnit_LilaStringsBase& U, const FControlRigExecuteContext& ExecuteContext, URigHierarchy* H, FSetup& Out)
{
	if (!H || U.Alpha <= 0.0f)
	{
		return false;
	}
	Out.H = H;
	Out.Web = Cached(U.RuleWeb);
	if (!Out.Web->Error.IsEmpty())
	{
		UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Out.Web->Error);
		return false;
	}
	if (!H->Contains(BoneKey(U.Bone)))
	{
		UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("No bone named %s"), *U.Bone.ToString());
		return false;
	}
	const FRigElementKey ControlKey(U.Control, ERigElementType::Control);
	Out.Goal = !U.Control.IsNone() && H->Contains(ControlKey) ? H->GetGlobalTransform(ControlKey) : U.Target;
	Out.Now = H->GetGlobalTransform(BoneKey(U.Bone));
	return true;
}
} // namespace

FRigUnit_LilaStringsForward_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsForward::Execute);

	FSetup S;
	if (!Prepare(*this, ExecuteContext, ExecuteContext.Hierarchy, S))
	{
		return;
	}
	FPoseBlend Blend;
	Blend.Capture(S.H, *S.Web);
	FTransform T = S.Now;
	T.SetRotation(S.Goal.GetRotation());
	S.H->SetGlobalTransform(BoneKey(Bone), T, false, true);
	Blend.Apply(S.H, Alpha);
	Error = 0.0f;
}

FRigUnit_LilaStringsBackward_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsBackward::Execute);

	FSetup S;
	if (!Prepare(*this, ExecuteContext, ExecuteContext.Hierarchy, S))
	{
		return;
	}
	const FRuleWeb& W = *S.Web;
	if (W.Hips.IsNone() || !S.H->Contains(BoneKey(W.Hips)))
	{
		UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("The rule web names no hips to swing"));
		return;
	}
	FPoseBlend Blend;
	Blend.Capture(S.H, W);
	FHolds Holds;
	if (bHoldContacts)
	{
		Holds.Capture(S.H, W, Bone);
	}
	// The whole body turns rigidly about the posed bone, so it keeps its place and takes the new rotation.
	const FQuat Delta = S.Goal.GetRotation() * S.Now.GetRotation().Inverse();
	const FVector Pivot = S.Now.GetLocation();
	FTransform Hips = S.H->GetGlobalTransform(BoneKey(W.Hips));
	Hips.SetLocation(Pivot + Delta.RotateVector(Hips.GetLocation() - Pivot));
	Hips.SetRotation(Delta * Hips.GetRotation());
	S.H->SetGlobalTransform(BoneKey(W.Hips), Hips, false, true);
	Error = float(Holds.Restore(S.H, W));
	Blend.Apply(S.H, Alpha);
}

FRigUnit_LilaStringsIK_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsIK::Execute);

	FSetup S;
	if (!Prepare(*this, ExecuteContext, ExecuteContext.Hierarchy, S))
	{
		return;
	}
	const FRuleWeb& W = *S.Web;
	FPoseBlend Blend;
	Blend.Capture(S.H, W);
	FHolds Holds;
	if (bHoldContacts)
	{
		Holds.Capture(S.H, W, Bone);
	}
	if (Bone == W.Hips)
	{
		// The hips carry the body: it moves and the contacts reach back.
		FTransform T = S.Now;
		T.SetLocation(S.Goal.GetLocation());
		if (bRotate)
		{
			T.SetRotation(S.Goal.GetRotation());
		}
		S.H->SetGlobalTransform(BoneKey(Bone), T, false, true);
	}
	else
	{
		SolveLimb(S.H, LimbOf(W, Bone, ChainLength), S.Goal.GetLocation());
		if (bRotate)
		{
			FTransform T = S.H->GetGlobalTransform(BoneKey(Bone));
			T.SetRotation(S.Goal.GetRotation());
			S.H->SetGlobalTransform(BoneKey(Bone), T, false, true);
		}
	}
	Error = float(Holds.Restore(S.H, W));
	Blend.Apply(S.H, Alpha);
}

FRigUnit_LilaStringsFullBody_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsFullBody::Execute);

	FSetup S;
	if (!Prepare(*this, ExecuteContext, ExecuteContext.Hierarchy, S))
	{
		return;
	}
	const FRuleWeb& W = *S.Web;
	FPoseBlend Blend;
	Blend.Capture(S.H, W);
	FHolds Holds;
	if (bHoldContacts)
	{
		Holds.Capture(S.H, W, Bone);
	}
	const TArray<FName> Limb = LimbOf(W, Bone);
	if (!W.Hips.IsNone() && S.H->Contains(BoneKey(W.Hips)) && Bone != W.Hips && !IsUnder(W, W.Hips, Bone))
	{
		// What the limb can't reach, the body brings: the hips move by the shortfall, less as they stiffen.
		double Reach = 0.0;
		for (int32 I = 0; I + 1 < Limb.Num(); ++I)
		{
			Reach += (S.H->GetGlobalTransform(BoneKey(Limb[I + 1])).GetLocation() - S.H->GetGlobalTransform(BoneKey(Limb[I])).GetLocation()).Size();
		}
		const FVector Top = S.H->GetGlobalTransform(BoneKey(Limb[0])).GetLocation();
		const FVector ToGoal = S.Goal.GetLocation() - Top;
		const double Short = ToGoal.Size() - Reach * 0.98;
		if (Short > 0.0)
		{
			const double Give = 1.0 - FMath::Clamp(W.Stiffness.FindRef(W.Hips), 0.0, 1.0);
			FTransform Hips = S.H->GetGlobalTransform(BoneKey(W.Hips));
			Hips.AddToTranslation(ToGoal.GetSafeNormal() * Short * Give);
			S.H->SetGlobalTransform(BoneKey(W.Hips), Hips, false, true);
		}
	}
	else if (Bone == W.Hips)
	{
		FTransform T = S.Now;
		T.SetLocation(S.Goal.GetLocation());
		S.H->SetGlobalTransform(BoneKey(Bone), T, false, true);
	}
	if (Bone != W.Hips)
	{
		SolveLimb(S.H, Limb, S.Goal.GetLocation());
	}
	if (bRotate)
	{
		FTransform T = S.H->GetGlobalTransform(BoneKey(Bone));
		T.SetRotation(S.Goal.GetRotation());
		S.H->SetGlobalTransform(BoneKey(Bone), T, false, true);
	}
	Error = float(Holds.Restore(S.H, W));
	Blend.Apply(S.H, Alpha);
}
