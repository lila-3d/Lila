#include "LilaLinkHeightNormal.h"

#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMaterials.h"

#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureProperty.h"
#include "Materials/MaterialFunction.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
constexpr int32 Version = 1;
const TCHAR* FunctionName = TEXT("MF_LilaHeightNormal");
const TCHAR* VersionTag = TEXT("Lila Height Normal v");

int32 VersionOf(const UMaterialFunction* Function)
{
	for (UMaterialExpression* E : Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(VersionTag))
		{
			return FCString::Atoi(*C->Description.RightChop(FCString::Strlen(VersionTag)));
		}
	}
	return 0;
}
} // namespace

FString LilaLink::HeightNormalHlsl(const TCHAR* Tex, const TCHAR* Uv, const TCHAR* Texel, const TCHAR* Slope, const TCHAR* Detail)
{
	// Central differences one height texel apart; v runs down the texture, as DirectX green does.
	return FString::Printf(TEXT(R"(
float2 hnE = max(%s, 1e-6);
float hnL = Texture2DSample(%s, %sSampler, %s - float2(hnE.x, 0)).r;
float hnR = Texture2DSample(%s, %sSampler, %s + float2(hnE.x, 0)).r;
float hnU = Texture2DSample(%s, %sSampler, %s - float2(0, hnE.y)).r;
float hnD = Texture2DSample(%s, %sSampler, %s + float2(0, hnE.y)).r;
float2 hnSlope = float2(hnR - hnL, hnD - hnU) / (2.0 * hnE) * %s;
float3 hnBase = normalize(float3(-hnSlope, 1.0));
float3 hnT = hnBase + float3(0, 0, 1);
float3 hnU2 = normalize(%s) * float3(-1, -1, 1);
float3 heightNormal = normalize(hnT * dot(hnT, hnU2) / hnT.z - hnU2);
)"),
		Texel, Tex, Tex, Uv, Tex, Tex, Uv, Tex, Tex, Uv, Tex, Tex, Uv, Slope, Detail);
}

UMaterialFunction* LilaLink::HeightNormalFunction(bool bRebuild, TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkHeightNormal::HeightNormalFunction);

	bool bCreated = false;
	UMaterialFunction* Function = FindOrCreateMaterialFunction(MastersFolderPath(), FunctionName, bCreated);
	if (!Function)
	{
		return nullptr;
	}
	if (bCreated)
	{
		OutCreated.AddUnique(Function->GetPathName());
	}
	if (!bCreated && !bRebuild && VersionOf(Function) >= Version)
	{
		return Function;
	}

	FFunctionGraph G(Function, TEXT("Lila Height Normal: the normal a height map's surface has, with a detail normal laid on it, so the "
									"walls of the relief light as walls. Feed it Lila's height and detail normal (bake detailNormal, or "
									"export with Normals From Height) and the depth and UV size they were made at."));
	UTexture* Black = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
	UMaterialExpression* Height = G.TextureInput(TEXT("HeightMap"), Black, SAMPLERTYPE_LinearGrayscale, TEXT("Height Normal"),
		TEXT("Lila's height map, 16 bit; 0.5 is the surface."));
	auto* Up = G.New<UMaterialExpressionConstant3Vector>(-1500);
	Up->Constant = FLinearColor(0.0f, 0.0f, 1.0f);
	UMaterialExpression* Detail = G.Input(TEXT("DetailNormal"), FunctionInput_Vector3, Up,
		TEXT("Tangent normal laid on top: a sampled detail normal map; flat when unconnected."));
	UMaterialExpression* Depth = G.ScalarInput(TEXT("HeightDepth"), 5.0f, TEXT("Height Normal"), TEXT("cm from height 0 to height 1."));
	UMaterialExpression* Width = G.ScalarInput(TEXT("HeightUVWidth"), 100.0f, TEXT("Height Normal"), TEXT("World size of the UV square along U, cm."));
	UMaterialExpression* Tall = G.ScalarInput(TEXT("HeightUVHeight"), 100.0f, TEXT("Height Normal"), TEXT("World size of the UV square along V, cm."));
	auto* UV0 = G.New<UMaterialExpressionTextureCoordinate>(-1500);
	UMaterialExpression* Uvs = G.Input(TEXT("UVs"), FunctionInput_Vector2, UV0, TEXT("The height map's UVs; UV channel 0 when unconnected."));

	auto* Texel = G.New<UMaterialExpressionTextureProperty>(-800);
	Texel->Property = TMTM_TexelSize;
	Texel->TextureObject.Connect(0, Height);
	auto* Size = G.New<UMaterialExpressionAppendVector>(-800);
	Size->A.Connect(0, Width);
	Size->B.Connect(0, Tall);

	auto* Custom = G.New<UMaterialExpressionCustom>(-400);
	Custom->Description = FString::Printf(TEXT("%s%d"), VersionTag, Version);
	Custom->Code = HeightNormalHlsl(TEXT("HeightTex"), TEXT("UV"), TEXT("Texel"), TEXT("Depth / max(Size, 1e-3)"), TEXT("Detail")) +
				   TEXT("return heightNormal;");
	Custom->OutputType = CMOT_Float3;
	Custom->Inputs.Reset();
	const TPair<const TCHAR*, UMaterialExpression*> Wires[] = {
		{TEXT("HeightTex"), Height}, {TEXT("UV"), Uvs}, {TEXT("Texel"), Texel}, {TEXT("Depth"), Depth}, {TEXT("Size"), Size}, {TEXT("Detail"), Detail}};
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : Wires)
	{
		Custom->Inputs.AddDefaulted_GetRef().InputName = W.Key;
	}
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : Wires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value, TEXT(""), Custom, W.Key);
	}
	G.Output(Custom, TEXT("Normal"), TEXT("Tangent-space normal: connect to the material's Normal."));
	G.Finish();
	return Function;
}
