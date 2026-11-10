#include "LilaSoftNetData.h"
#include "LilaSoftNetDataInterface.h"
#include "LilaGraphPins.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "OptimusComponentSource.h"
#include "OptimusDeformer.h"
#include "OptimusDataDomain.h"
#include "OptimusDiagnostic.h"
#include "OptimusExecutionDomain.h"
#include "OptimusShaderText.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace
{
// The kernel: linear skinning as the stock graph does, then the net's offset. Tangents turn by the deformation gradient
// (the skin's matrix plus the offset's derivative), normals by its cofactor, as Lila's rebuilt normals do.
const TCHAR* const SoftNetKernel = TEXT(R"(if (Index >= ReadNumThreads().x) return;
const float3x4 M = ReadWeightedBoneMatrix(Index);
const float3 P = mul(M, float4(ReadRestPosition(Index), 1.0));
WritePosition(Index, P + ReadNetOffset(Index));
const float3 F0 = float3(M[0][0], M[1][0], M[2][0]) + ReadNetJacobianX(Index);
const float3 F1 = float3(M[0][1], M[1][1], M[2][1]) + ReadNetJacobianY(Index);
const float3 F2 = float3(M[0][2], M[1][2], M[2][2]) + ReadNetJacobianZ(Index);
const float4 TX = ReadRestTangentX(Index);
const float4 TZ = ReadRestTangentZ(Index);
WriteTangentX(Index, float4(normalize(F0 * TX.x + F1 * TX.y + F2 * TX.z), TX.w));
WriteTangentZ(Index, float4(normalize(cross(F1, F2) * TZ.x + cross(F2, F0) * TZ.y + cross(F0, F1) * TZ.z), TZ.w));
)");

#if WITH_EDITOR
/** True when the graph's custom kernel runs this plugin's current kernel source. */
bool RunsCurrentKernel(UOptimusDeformer* Deformer, UClass* KernelClass)
{
	FStructProperty* Source = KernelClass ? CastField<FStructProperty>(KernelClass->FindPropertyByName(TEXT("ShaderSource"))) : nullptr;
	UOptimusNodeGraph* Graph = Deformer->GetUpdateGraph();
	if (!Source || Source->Struct != FOptimusShaderText::StaticStruct() || !Graph)
	{
		return false;
	}
	for (UOptimusNode* Node : Graph->GetAllNodes())
	{
		if (Node && Node->IsA(KernelClass))
		{
			return Source->ContainerPtrToValuePtr<FOptimusShaderText>(Node)->ShaderText == SoftNetKernel;
		}
	}
	return false;
}
#endif
} // namespace

UObject* ULilaSoftNetLibrary::MakeSoftNetDeformer(const FString& AssetPath, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaSoftNetLibrary::MakeSoftNetDeformer);

#if WITH_EDITOR
	using namespace LilaGraphPins;
	UClass* KernelClass = FindObject<UClass>(nullptr, TEXT("/Script/OptimusCore.OptimusNode_CustomComputeKernel"));
	if (UOptimusDeformer* Existing = LoadObject<UOptimusDeformer>(nullptr, *AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		if (RunsCurrentKernel(Existing, KernelClass))
		{
			return Existing;
		}
		// A graph from an older LilaLink: moved aside and made again under its name.
		Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	UPackage* Package = CreatePackage(*PackageName);
	UOptimusDeformer* Deformer = NewObject<UOptimusDeformer>(Package, FName(*FPackageName::GetShortName(PackageName)), RF_Public | RF_Standalone | RF_Transactional);
	Deformer->AddComponentBinding(nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName());
	UOptimusNodeGraph* Graph = Deformer->GetUpdateGraph();
	UClass* SkinnedMeshClass = FindObject<UClass>(nullptr, TEXT("/Script/OptimusCore.OptimusSkinnedMeshDataInterface"));
	UClass* SkeletonClass = FindObject<UClass>(nullptr, TEXT("/Script/OptimusCore.OptimusSkeletonDataInterface"));
	UClass* WriteClass = FindObject<UClass>(nullptr, TEXT("/Script/OptimusCore.OptimusSkinnedMeshWriteDataInterface"));
	if (!Graph || !SkinnedMeshClass || !SkeletonClass || !WriteClass || !KernelClass)
	{
		OutError = TEXT("this engine's Deformer Graph lacks the skinned mesh or skeleton interfaces or the custom kernel node");
		return nullptr;
	}
	// Off the origin: a node placed where it already is counts as not placed, and is dropped.
	UOptimusNode* Mesh = Graph->AddDataInterfaceNode(SkinnedMeshClass, FVector2D(0, -100));
	UOptimusNode* Skeleton = Graph->AddDataInterfaceNode(SkeletonClass, FVector2D(0, 150));
	UOptimusNode* Net = Graph->AddDataInterfaceNode(ULilaSoftNetDataInterface::StaticClass(), FVector2D(0, 350));
	UOptimusNode* Write = Graph->AddDataInterfaceNode(WriteClass, FVector2D(900, 100));
	UOptimusNode* Kernel = Graph->AddNode(KernelClass, FVector2D(450, 100));
	if (!Mesh || !Skeleton || !Net || !Write || !Kernel)
	{
		OutError = TEXT("couldn't add the graph's nodes to ") + Graph->GetCollectionPath();
		return nullptr;
	}
	const bool bPins = AddKernelPin(Graph, Kernel, PinNamed(Mesh, TEXT("Position")), EOptimusNodePinDirection::Input, "RestPosition", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Mesh, TEXT("TangentX")), EOptimusNodePinDirection::Input, "RestTangentX", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Mesh, TEXT("TangentZ")), EOptimusNodePinDirection::Input, "RestTangentZ", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Skeleton, TEXT("WeightedBoneMatrix")), EOptimusNodePinDirection::Input, "WeightedBoneMatrix", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Net, TEXT("NetOffset")), EOptimusNodePinDirection::Input, "NetOffset", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Net, TEXT("NetJacobianX")), EOptimusNodePinDirection::Input, "NetJacobianX", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Net, TEXT("NetJacobianY")), EOptimusNodePinDirection::Input, "NetJacobianY", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Net, TEXT("NetJacobianZ")), EOptimusNodePinDirection::Input, "NetJacobianZ", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Write, TEXT("Position")), EOptimusNodePinDirection::Output, "Position", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Write, TEXT("TangentX")), EOptimusNodePinDirection::Output, "TangentX", OutError) &&
					   AddKernelPin(Graph, Kernel, PinNamed(Write, TEXT("TangentZ")), EOptimusNodePinDirection::Output, "TangentZ", OutError);
	if (!bPins)
	{
		return nullptr;
	}
	FStructProperty* Source = CastField<FStructProperty>(KernelClass->FindPropertyByName(TEXT("ShaderSource")));
	FStructProperty* Domain = CastField<FStructProperty>(KernelClass->FindPropertyByName(TEXT("ExecutionDomain")));
	if (!Source || Source->Struct != FOptimusShaderText::StaticStruct() || !Domain || Domain->Struct != FOptimusExecutionDomain::StaticStruct())
	{
		OutError = TEXT("the custom kernel node has no ShaderSource or ExecutionDomain");
		return nullptr;
	}
	Kernel->PreEditChange(Source);
	Source->ContainerPtrToValuePtr<FOptimusShaderText>(Kernel)->ShaderText = SoftNetKernel;
	FPropertyChangedEvent Changed(Source);
	Kernel->PostEditChangeProperty(Changed);
	Kernel->PreEditChange(Domain);
	*Domain->ContainerPtrToValuePtr<FOptimusExecutionDomain>(Kernel) = FOptimusExecutionDomain(Optimus::DomainName::Vertex);
	FPropertyChangedEvent DomainChanged(Domain);
	Kernel->PostEditChangeProperty(DomainChanged);
	TArray<FString> Messages;
	const FDelegateHandle Listen = Deformer->GetCompileMessageDelegate().AddLambda([&Messages](const FOptimusCompilerDiagnostic& D) {
		if (D.Level == EOptimusDiagnosticLevel::Error)
		{
			Messages.Add(D.Message.ToString());
		}
	});
	const bool bCompiled = Deformer->Compile();
	Deformer->GetCompileMessageDelegate().Remove(Listen);
	if (!bCompiled || !Messages.IsEmpty())
	{
		OutError = TEXT("the graph didn't compile: ") + FString::Join(Messages, TEXT("; "));
	}
	FAssetRegistryModule::AssetCreated(Deformer);
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, Deformer, *FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Args);
	return Deformer;
#else
	OutError = TEXT("the deformer is made in the editor");
	return nullptr;
#endif
}
