#include "LilaLinkExpressions.h"

#include "LilaLinkLog.h"
#include "LilaLinkMethods.h"

#include "AnimGraphNode_ControlRig.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AssetToolsModule.h"
#include "ControlRig.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "IAssetTools.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
using namespace LilaLink;

/** The pose pin of a node in a direction: anim graph nodes pass poses as FPoseLink structs. */
UEdGraphPin* PosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
			Pin->PinType.PinSubCategoryObject == FPoseLink::StaticStruct())
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraph* FindAnimGraph(UAnimBlueprint* Blueprint)
{
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
		{
			return Graph;
		}
	}
	return nullptr;
}

/**
 * expressionPostProcess {skeletalMesh, rig, animBlueprint?}: runs the Control Rig from the mesh's post-process Anim
 * Blueprint (Input Pose -> Control Rig -> Output Pose), so the expressions run wherever the mesh plays, and makes it
 * the mesh's default animating rig for Sequencer. A post-process Anim Blueprint that isn't ours is left alone.
 */
TSharedPtr<FJsonValue> ExpressionPostProcess(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkExpressions::ExpressionPostProcess);

	const FString MeshPath = Params->GetStringField(TEXT("skeletalMesh"));
	const FString RigPath = Params->GetStringField(TEXT("rig"));
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	UBlueprint* Rig = LoadObject<UBlueprint>(nullptr, *RigPath);
	if (!Mesh || !Mesh->GetSkeleton())
	{
		OutError = FString::Printf(TEXT("expressionPostProcess: no skeletal mesh at %s"), *MeshPath);
		return nullptr;
	}
	if (!Rig || !Rig->GeneratedClass || !Rig->GeneratedClass->IsChildOf(UControlRig::StaticClass()))
	{
		OutError = FString::Printf(TEXT("expressionPostProcess: no Control Rig at %s"), *RigPath);
		return nullptr;
	}
	FString AbpPath;
	if (!Params->TryGetStringField(TEXT("animBlueprint"), AbpPath) || AbpPath.IsEmpty())
	{
		FString Folder = FPackageName::GetLongPackagePath(RigPath);
		FString Name = FPackageName::GetShortName(RigPath);
		Name.RemoveFromStart(TEXT("CR_"));
		AbpPath = Folder / TEXT("ABP_") + Name;
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Warnings;
	Mesh->Modify();
	Mesh->SetDefaultAnimatingRig(TSoftObjectPtr<UObject>(Rig));

	const UClass* Existing = Mesh->GetPostProcessAnimBlueprint().Get();
	const FString ExistingPath = Existing ? FPackageName::ObjectPathToPackageName(Existing->GetPathName()) : FString();
	if (Existing && ExistingPath != AbpPath)
	{
		Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("%s already has the post-process Anim Blueprint %s: add a Control Rig node running %s to it"), *MeshPath, *ExistingPath, *RigPath)));
		Out->SetArrayField(TEXT("warnings"), Warnings);
		TArray<FString> Errors;
		LilaLink::SaveAssetPaths({MeshPath}, Errors);
		return MakeShared<FJsonValueObject>(Out);
	}

	UAnimBlueprint* Abp = LoadObject<UAnimBlueprint>(nullptr, *AbpPath);
	if (!Abp)
	{
		UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
		Factory->TargetSkeleton = Mesh->GetSkeleton();
		Factory->PreviewSkeletalMesh = Mesh;
		Factory->ParentClass = UAnimInstance::StaticClass();
		Factory->BlueprintType = BPTYPE_Normal;
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		Abp = Cast<UAnimBlueprint>(Tools.CreateAsset(FPackageName::GetShortName(AbpPath), FPackageName::GetLongPackagePath(AbpPath),
		                                             UAnimBlueprint::StaticClass(), Factory));
		if (!Abp)
		{
			OutError = FString::Printf(TEXT("expressionPostProcess: couldn't create %s"), *AbpPath);
			return nullptr;
		}
	}
	UEdGraph* Graph = FindAnimGraph(Abp);
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("expressionPostProcess: %s has no AnimGraph"), *AbpPath);
		return nullptr;
	}

	UAnimGraphNode_Root* Root = nullptr;
	UAnimGraphNode_ControlRig* RigNode = nullptr;
	UAnimGraphNode_LinkedInputPose* Input = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		Root = Root ? Root : Cast<UAnimGraphNode_Root>(Node);
		RigNode = RigNode ? RigNode : Cast<UAnimGraphNode_ControlRig>(Node);
		Input = Input ? Input : Cast<UAnimGraphNode_LinkedInputPose>(Node);
	}
	if (!Root)
	{
		OutError = FString::Printf(TEXT("expressionPostProcess: %s has no Output Pose"), *AbpPath);
		return nullptr;
	}
	if (!Input)
	{
		FGraphNodeCreator<UAnimGraphNode_LinkedInputPose> Creator(*Graph);
		Input = Creator.CreateNode();
		Input->NodePosX = Root->NodePosX - 600;
		Input->NodePosY = Root->NodePosY;
		Creator.Finalize();
	}
	if (!RigNode)
	{
		FGraphNodeCreator<UAnimGraphNode_ControlRig> Creator(*Graph);
		RigNode = Creator.CreateNode();
		RigNode->NodePosX = Root->NodePosX - 300;
		RigNode->NodePosY = Root->NodePosY;
		Creator.Finalize();
	}
	RigNode->Modify();
	RigNode->Node.SetControlRigClass(Rig->GeneratedClass.Get());
	RigNode->ReconstructNode();

	UEdGraphPin* InputOut = PosePin(Input, EGPD_Output);
	UEdGraphPin* RigIn = PosePin(RigNode, EGPD_Input);
	UEdGraphPin* RigOut = PosePin(RigNode, EGPD_Output);
	UEdGraphPin* RootIn = PosePin(Root, EGPD_Input);
	if (!InputOut || !RigIn || !RigOut || !RootIn)
	{
		OutError = TEXT("expressionPostProcess: an anim graph node has no pose pin");
		return nullptr;
	}
	RigIn->BreakAllPinLinks();
	RootIn->BreakAllPinLinks();
	InputOut->MakeLinkTo(RigIn);
	RigOut->MakeLinkTo(RootIn);

	FKismetEditorUtilities::CompileBlueprint(Abp);
	if (Abp->Status == BS_Error)
	{
		OutError = FString::Printf(TEXT("expressionPostProcess: %s doesn't compile"), *AbpPath);
		return nullptr;
	}
	Mesh->SetPostProcessAnimBlueprint(TSubclassOf<UAnimInstance>(Abp->GeneratedClass.Get()));
	Mesh->PostEditChange();

	TArray<FString> Errors;
	LilaLink::SaveAssetPaths({AbpPath, MeshPath}, Errors);
	for (const FString& E : Errors)
	{
		Warnings.Add(MakeShared<FJsonValueString>(E));
	}
	Out->SetStringField(TEXT("animBlueprint"), AbpPath);
	Out->SetArrayField(TEXT("warnings"), Warnings);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddExpressionMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("expressionPostProcess"), LilaLink::Sync(&ExpressionPostProcess));
}
