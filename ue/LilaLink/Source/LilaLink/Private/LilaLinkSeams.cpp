#include "LilaLinkSeams.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethods.h"

#include "Components/DecalComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/DecalActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialFunction.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
constexpr int32 Version = 1;
const TCHAR* VersionTag = TEXT("Lila Seams v");
const TCHAR* DecalTagPrefix = TEXT("LilaSeam:");
const TCHAR* DecalGroupPrefix = TEXT("LilaSeamGroup:");

TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }
TSharedPtr<FJsonValue> Str(const FString& S) { return MakeShared<FJsonValueString>(S); }

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

/** A Custom node running Code over named inputs; the first one carries the version tag that VersionOf reads. */
UMaterialExpressionCustom* Custom(LilaLink::FFunctionGraph& G, const FString& Description, const FString& Code, ECustomMaterialOutputType Type,
	std::initializer_list<TPair<const TCHAR*, TPair<UMaterialExpression*, const TCHAR*>>> Wires, int32 X = -400)
{
	auto* E = G.New<UMaterialExpressionCustom>(X);
	E->Description = FString::Printf(TEXT("%s%d %s"), VersionTag, Version, *Description);
	E->Code = Code;
	E->OutputType = Type;
	E->Inputs.Reset();
	for (const auto& W : Wires)
	{
		E->Inputs.AddDefaulted_GetRef().InputName = W.Key;
	}
	for (const auto& W : Wires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value.Key, W.Value.Value, E, W.Key);
	}
	return E;
}

TPair<UMaterialExpression*, const TCHAR*> Pin(UMaterialExpression* E, const TCHAR* Output = TEXT("")) { return {E, Output}; }

/**
 * MF_LilaContactMask: the contact weight Lila's field modifier wrote (a vertex colour channel, or U of a UV set), shaped
 * by strength and contrast, blends a base and a contact colour and roughness; an optional pixel depth offset dither, seeded
 * per instance so neighbours' dithers don't cancel, desktop only (needs TAA; nothing on mobile).
 */
void BuildContactMask(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Contact Mask: grime, moss or wet lines where a mesh meets the ground or another object, "
											  "from the weight Lila wrote into a vertex colour channel or UV set. No GBuffer or depth reads."));
	UMaterialExpression* Base = G.VectorInput(TEXT("BaseColor"), FLinearColor(0.5f, 0.5f, 0.5f), TEXT("Contact"), TEXT("The surface's own colour."));
	UMaterialExpression* ContactColor = G.VectorInput(TEXT("ContactColor"), FLinearColor(0.12f, 0.1f, 0.07f), TEXT("Contact"), TEXT("Colour at the contact."));
	UMaterialExpression* BaseRough = G.ScalarInput(TEXT("BaseRoughness"), 0.7f, TEXT("Contact"), TEXT("The surface's own roughness."));
	UMaterialExpression* ContactRough = G.ScalarInput(TEXT("ContactRoughness"), 0.9f, TEXT("Contact"), TEXT("Roughness at the contact."));
	UMaterialExpression* Channel = G.ScalarInput(TEXT("ContactChannel"), 0.0f, TEXT("Contact"), TEXT("Vertex colour channel: 0 R, 1 G, 2 B, 3 A."));
	UMaterialExpression* FromUV = G.ScalarInput(TEXT("ContactFromUV"), 0.0f, TEXT("Contact"), TEXT("1 reads U of ContactUVs instead of vertex colour."));
	UMaterialExpression* Strength = G.ScalarInput(TEXT("ContactStrength"), 1.0f, TEXT("Contact"), TEXT("Scales the weight."));
	UMaterialExpression* Contrast = G.ScalarInput(TEXT("ContactContrast"), 1.0f, TEXT("Contact"), TEXT("Power on the weight: above 1 tightens the band."));
	UMaterialExpression* Dither = G.ScalarInput(TEXT("ContactDitherDepth"), 0.0f, TEXT("Contact"),
		TEXT("Pixel depth offset dither at the contact, cm; 0 is off. Desktop with TAA only."));
	auto* UV2 = G.New<UMaterialExpressionTextureCoordinate>(-1700);
	UV2->CoordinateIndex = 2;
	UMaterialExpression* Uvs = G.Input(TEXT("ContactUVs"), FunctionInput_Vector2, UV2, TEXT("UV set Lila wrote with field='uv'; UV channel 2 when unconnected."));
	auto* ObjPos = G.New<UMaterialExpressionObjectPositionWS>(-1700);
	UMaterialExpression* SeedDefault = Custom(G, TEXT("seed"), TEXT("return frac(dot(P, float3(0.1031, 0.1030, 0.0973)));"), CMOT_Float1, {{TEXT("P"), Pin(ObjPos)}}, -1400);
	UMaterialExpression* Seed = G.Input(TEXT("Seed"), FunctionInput_Scalar, SeedDefault, TEXT("Per-instance dither seed; the object's position when unconnected."));
	auto* VC = G.New<UMaterialExpressionVertexColor>(-1700);

	UMaterialExpression* Mask = Custom(G, TEXT("mask"),
		TEXT("float4 c = float4(VC, VA);\n"
			 "int ch = (int)clamp(round(Channel), 0, 3);\n"
			 "float raw = FromUV > 0.5 ? UV.x : (ch == 0 ? c.r : ch == 1 ? c.g : ch == 2 ? c.b : c.a);\n"
			 "return saturate(pow(saturate(raw), max(Contrast, 1e-3)) * Strength);"),
		CMOT_Float1,
		{{TEXT("VC"), Pin(VC)}, {TEXT("VA"), Pin(VC, TEXT("A"))}, {TEXT("Channel"), Pin(Channel)}, {TEXT("FromUV"), Pin(FromUV)}, {TEXT("UV"), Pin(Uvs)},
			{TEXT("Contrast"), Pin(Contrast)}, {TEXT("Strength"), Pin(Strength)}});
	UMaterialExpression* Color = Custom(G, TEXT("colour"), TEXT("return lerp(A, B, M);"), CMOT_Float3,
		{{TEXT("A"), Pin(Base)}, {TEXT("B"), Pin(ContactColor)}, {TEXT("M"), Pin(Mask)}}, -150);
	UMaterialExpression* Rough = Custom(G, TEXT("roughness"), TEXT("return lerp(A, B, M);"), CMOT_Float1,
		{{TEXT("A"), Pin(BaseRough)}, {TEXT("B"), Pin(ContactRough)}, {TEXT("M"), Pin(Mask)}}, -150);
	auto* Screen = G.New<UMaterialExpressionScreenPosition>(-700);
	UMaterialExpression* Pdo = Custom(G, TEXT("dither"),
		TEXT("float n = frac(sin(dot(SP * 1000.0 + Seed * 101.0, float2(12.9898, 78.233))) * 43758.5453);\n"
			 "return M * Depth * n;"),
		CMOT_Float1, {{TEXT("SP"), Pin(Screen, TEXT("ViewportUV"))}, {TEXT("Seed"), Pin(Seed)}, {TEXT("M"), Pin(Mask)}, {TEXT("Depth"), Pin(Dither)}}, -150);
	auto* Zero = G.New<UMaterialExpressionConstant>(-150);
	Zero->R = 0.0f;
	G.Output(Mask, TEXT("Mask"), TEXT("Contact weight, 0 to 1."));
	G.Output(Color, TEXT("BaseColor"), TEXT("BaseColor and ContactColor blended by the mask."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Roughness blended by the mask."));
	G.Output(G.FeatureLevelSwitch(Pdo, Zero), TEXT("PixelDepthOffset"), TEXT("Dither for the material's Pixel Depth Offset; 0 on mobile."));
	G.Finish();
}

/**
 * MF_LilaSeamStrip: a strip texture along a contact strip's UVs (U runs along the strip in repeats, V across it), faded by
 * the vertex alpha Lila wrote, for masked or translucent strips and decals.
 */
void BuildStrip(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Seam Strip: a dirt, moss or grime texture along a contact strip, its edge fading by the "
											  "strip's vertex alpha. Works masked, translucent or as a decal."));
	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UMaterialExpression* Tex = G.TextureInput(TEXT("StripTexture"), White, SAMPLERTYPE_Color, TEXT("Strip"), TEXT("Colour in RGB, coverage in A."));
	UMaterialExpression* Tint = G.VectorInput(TEXT("StripTint"), FLinearColor(0.25f, 0.2f, 0.15f), TEXT("Strip"), TEXT("Multiplies the texture."));
	UMaterialExpression* Rough = G.ScalarInput(TEXT("StripRoughness"), 0.9f, TEXT("Strip"), TEXT("Roughness."));
	UMaterialExpression* Fade = G.ScalarInput(TEXT("StripFade"), 1.0f, TEXT("Strip"), TEXT("Power on the vertex alpha: above 1 thins the edge."));
	UMaterialExpression* Opacity = G.ScalarInput(TEXT("StripOpacity"), 1.0f, TEXT("Strip"), TEXT("Scales the coverage."));
	auto* UV0 = G.New<UMaterialExpressionTextureCoordinate>(-1700);
	UMaterialExpression* Uvs = G.Input(TEXT("StripUVs"), FunctionInput_Vector2, UV0, TEXT("The strip's UVs; UV channel 0 when unconnected."));
	auto* VC = G.New<UMaterialExpressionVertexColor>(-1700);
	UMaterialExpression* Color = Custom(G, TEXT("colour"), TEXT("return Texture2DSample(StripTex, StripTexSampler, UV).rgb * Tint;"), CMOT_Float3,
		{{TEXT("StripTex"), Pin(Tex)}, {TEXT("UV"), Pin(Uvs)}, {TEXT("Tint"), Pin(Tint)}});
	UMaterialExpression* Alpha = Custom(G, TEXT("opacity"),
		TEXT("return saturate(Texture2DSample(StripTex, StripTexSampler, UV).a * pow(saturate(VA), max(Fade, 1e-3)) * Opacity);"), CMOT_Float1,
		{{TEXT("StripTex"), Pin(Tex)}, {TEXT("UV"), Pin(Uvs)}, {TEXT("VA"), Pin(VC, TEXT("A"))}, {TEXT("Fade"), Pin(Fade)}, {TEXT("Opacity"), Pin(Opacity)}});
	G.Output(Color, TEXT("BaseColor"), TEXT("Strip colour."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Roughness."));
	G.Output(Alpha, TEXT("Opacity"), TEXT("Coverage: to Opacity Mask (masked) or Opacity (translucent, decal)."));
	G.Finish();
}

/**
 * MF_LilaSkirtBlend: lerps a wall material and the ground's by the skirt's side (vertex alpha, 0 at the wall border, 1 at the
 * ground border), broken up by world noise on desktop. The ground colour defaults to the terrain colour Lila baked into the
 * skirt's vertex RGB; an RVT sample can feed GroundBaseColor instead.
 */
void BuildSkirt(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Skirt Blend: blends a wall into the ground across Lila's contact skirt mesh by its side "
											  "attribute, sampling both materials itself, so it needs no GBuffer or depth reads."));
	UMaterialExpression* WallColor = G.VectorInput(TEXT("WallBaseColor"), FLinearColor(0.5f, 0.5f, 0.5f), TEXT("Skirt"), TEXT("The wall's colour."));
	UMaterialExpression* WallRough = G.ScalarInput(TEXT("WallRoughness"), 0.7f, TEXT("Skirt"), TEXT("The wall's roughness."));
	UMaterialExpression* GroundRough = G.ScalarInput(TEXT("GroundRoughness"), 0.9f, TEXT("Skirt"), TEXT("The ground's roughness."));
	UMaterialExpression* NoiseAmount = G.ScalarInput(TEXT("SkirtNoise"), 0.3f, TEXT("Skirt"), TEXT("How much world noise moves the blend line, 0 to 1."));
	UMaterialExpression* NoiseScale = G.ScalarInput(TEXT("SkirtNoiseScale"), 40.0f, TEXT("Skirt"), TEXT("Size of the noise, cm."));
	UMaterialExpression* Contrast = G.ScalarInput(TEXT("SkirtContrast"), 2.0f, TEXT("Skirt"), TEXT("Sharpness of the blend line."));
	auto* VC = G.New<UMaterialExpressionVertexColor>(-1700);
	UMaterialExpression* GroundColor = G.Input(TEXT("GroundBaseColor"), FunctionInput_Vector3, VC,
		TEXT("The ground's colour; the terrain colour Lila baked into vertex RGB when unconnected (or an RVT sample)."));
	UMaterialExpression* Side = G.Input(TEXT("Side"), FunctionInput_Scalar, VC, TEXT("0 at the wall border, 1 at the ground; vertex alpha when unconnected."), 4);
	auto* World = G.New<UMaterialExpressionWorldPosition>(-1700);
	UMaterialExpression* Noisy = Custom(G, TEXT("blend"),
		TEXT("float3 p = W / max(Scale, 1.0);\n"
			 "float3 i = floor(p); float3 f = frac(p); f = f * f * (3.0 - 2.0 * f);\n"
			 "float n = 0.0;\n"
			 "[unroll] for (int k = 0; k < 8; ++k) {\n"
			 "  float3 o = float3(k & 1, (k >> 1) & 1, (k >> 2) & 1);\n"
			 "  float h = frac(sin(dot(i + o, float3(127.1, 311.7, 74.7))) * 43758.5453);\n"
			 "  float3 w = lerp(1.0 - f, f, o);\n"
			 "  n += h * w.x * w.y * w.z;\n"
			 "}\n"
			 "float s = saturate(S + (n - 0.5) * Amount);\n"
			 "return saturate((s - 0.5) * Contrast + 0.5);"),
		CMOT_Float1,
		{{TEXT("W"), Pin(World)}, {TEXT("Scale"), Pin(NoiseScale)}, {TEXT("S"), Pin(Side)}, {TEXT("Amount"), Pin(NoiseAmount)}, {TEXT("Contrast"), Pin(Contrast)}});
	UMaterialExpression* Plain = Custom(G, TEXT("blend mobile"), TEXT("return saturate((saturate(S) - 0.5) * Contrast + 0.5);"), CMOT_Float1,
		{{TEXT("S"), Pin(Side)}, {TEXT("Contrast"), Pin(Contrast)}});
	UMaterialExpression* Blend = G.FeatureLevelSwitch(Noisy, Plain);
	UMaterialExpression* Color = Custom(G, TEXT("colour"), TEXT("return lerp(A, B, M);"), CMOT_Float3,
		{{TEXT("A"), Pin(WallColor)}, {TEXT("B"), Pin(GroundColor)}, {TEXT("M"), Pin(Blend)}}, -150);
	UMaterialExpression* Rough = Custom(G, TEXT("roughness"), TEXT("return lerp(A, B, M);"), CMOT_Float1,
		{{TEXT("A"), Pin(WallRough)}, {TEXT("B"), Pin(GroundRough)}, {TEXT("M"), Pin(Blend)}}, -150);
	G.Output(Color, TEXT("BaseColor"), TEXT("Wall and ground colour blended across the skirt."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Roughness blended across the skirt."));
	G.Output(Blend, TEXT("Blend"), TEXT("0 wall to 1 ground, for blending your own layers."));
	G.Finish();
}

void Wire(UMaterial* Material, UMaterialFunction* Function, std::initializer_list<TPair<const TCHAR*, EMaterialProperty>> Outputs, TArray<FString>& OutWarnings)
{
	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, Outputs))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

struct FSpec
{
	const TCHAR* Key;
	const TCHAR* FunctionName;
	const TCHAR* MaterialName;
	void (*Build)(UMaterialFunction*);
	TFunction<void(UMaterial*, UMaterialFunction*, TArray<FString>&)> Master;
};

TArray<FSpec> Specs()
{
	TArray<FSpec> S;
	S.Add({TEXT("contactMask"), TEXT("MF_LilaContactMask"), TEXT("M_LilaContactMask"), &BuildContactMask,
		[](UMaterial* M, UMaterialFunction* F, TArray<FString>& W) {
			Wire(M, F, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}, {TEXT("PixelDepthOffset"), MP_PixelDepthOffset}}, W);
		}});
	S.Add({TEXT("seamStrip"), TEXT("MF_LilaSeamStrip"), TEXT("M_LilaSeamStrip"), &BuildStrip,
		[](UMaterial* M, UMaterialFunction* F, TArray<FString>& W) {
			M->BlendMode = BLEND_Masked;
			M->OpacityMaskClipValue = 0.33f;
			Wire(M, F, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}, {TEXT("Opacity"), MP_OpacityMask}}, W);
		}});
	S.Add({TEXT("seamStripTranslucent"), TEXT("MF_LilaSeamStrip"), TEXT("M_LilaSeamStripTranslucent"), &BuildStrip,
		[](UMaterial* M, UMaterialFunction* F, TArray<FString>& W) {
			M->BlendMode = BLEND_Translucent;
			Wire(M, F, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}, {TEXT("Opacity"), MP_Opacity}}, W);
		}});
	S.Add({TEXT("seamDecal"), TEXT("MF_LilaSeamStrip"), TEXT("M_LilaSeamDecal"), &BuildStrip,
		[](UMaterial* M, UMaterialFunction* F, TArray<FString>& W) {
			M->MaterialDomain = MD_DeferredDecal;
			M->BlendMode = BLEND_Translucent;
			Wire(M, F, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}, {TEXT("Opacity"), MP_Opacity}}, W);
		}});
	S.Add({TEXT("skirtBlend"), TEXT("MF_LilaSkirtBlend"), TEXT("M_LilaSkirtBlend"), &BuildSkirt,
		[](UMaterial* M, UMaterialFunction* F, TArray<FString>& W) { Wire(M, F, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}}, W); }});
	return S;
}

/** seamMaterials {which?: [keys], regenerate?}: builds what is missing or older, returns {functions, materials, created, warnings}. */
TSharedPtr<FJsonValue> SeamMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSeams::SeamMaterials);

	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	TSet<FString> Which;
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (Params->TryGetArrayField(TEXT("which"), List))
	{
		for (const TSharedPtr<FJsonValue>& V : *List)
		{
			Which.Add(V->AsString());
		}
	}
	TArray<FString> Created, Warnings;
	TSet<UMaterialFunction*> Built;
	TSharedRef<FJsonObject> Functions = MakeShared<FJsonObject>(), Materials = MakeShared<FJsonObject>();
	for (const FSpec& S : Specs())
	{
		if (!Which.IsEmpty() && !Which.Contains(S.Key))
		{
			continue;
		}
		FString Error;
		const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), S.FunctionName, S.MaterialName, Error);
		if (!Pair.Material)
		{
			Warnings.Add(Error);
			continue;
		}
		const bool bStale = VersionOf(Pair.Function) < Version;
		if (Pair.bCreated || bRegenerate || bStale)
		{
			if (!Built.Contains(Pair.Function))
			{
				S.Build(Pair.Function);
				Built.Add(Pair.Function);
			}
			S.Master(Pair.Material, Pair.Function, Warnings);
			Created.AddUnique(Pair.Function->GetPathName());
			Created.AddUnique(Pair.Material->GetPathName());
		}
		Functions->SetStringField(S.Key, Pair.Function->GetPathName());
		Materials->SetStringField(S.Key, Pair.Material->GetPathName());
	}
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("functions"), Functions);
	Out->SetObjectField(TEXT("materials"), Materials);
	TArray<TSharedPtr<FJsonValue>> C, W;
	for (const FString& P : Created)
	{
		C.Add(Str(P));
	}
	for (const FString& P : Warnings)
	{
		W.Add(Str(P));
		UE_LOG(LogLilaLink, Warning, TEXT("seamMaterials: %s"), *P);
	}
	Out->SetArrayField(TEXT("created"), C);
	Out->SetArrayField(TEXT("warnings"), W);
	return Obj(Out);
}

FVector VecOf(const TSharedPtr<FJsonObject>& O, const TCHAR* Field, const FVector& Fallback)
{
	const TArray<TSharedPtr<FJsonValue>>* V = nullptr;
	if (!O->TryGetArrayField(Field, V) || V->Num() < 3)
	{
		return Fallback;
	}
	return FVector((*V)[0]->AsNumber(), (*V)[1]->AsNumber(), (*V)[2]->AsNumber());
}

/**
 * seamDecals {group, material?, decals: [{key, location, xAxis, zAxis, size}]} in Unreal space: one decal actor per entry,
 * keyed so a resend moves it; the group's actors missing from the list are removed.
 */
TSharedPtr<FJsonValue> SeamDecals(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSeams::SeamDecals);

	FString Group, MaterialPath;
	const TArray<TSharedPtr<FJsonValue>>* Decals = nullptr;
	if (!Params->TryGetStringField(TEXT("group"), Group) || !Params->TryGetArrayField(TEXT("decals"), Decals))
	{
		OutError = TEXT("seamDecals: give group and decals");
		return nullptr;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = TEXT("seamDecals: no editor world");
		return nullptr;
	}
	TArray<FString> Warnings;
	UMaterialInterface* Material = nullptr;
	if (Params->TryGetStringField(TEXT("material"), MaterialPath) && !MaterialPath.IsEmpty())
	{
		Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	}
	if (!Material)
	{
		const TSharedPtr<FJsonObject> Which = MakeShared<FJsonObject>();
		Which->SetArrayField(TEXT("which"), {Str(TEXT("seamDecal"))});
		FString Ignored;
		SeamMaterials(Which, Ignored);
		Material = LoadObject<UMaterialInterface>(nullptr, *(FString(LilaLink::MastersFolderPath()) + TEXT("/M_LilaSeamDecal.M_LilaSeamDecal")));
	}
	const FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SeamDecals", "Lila contact decals"));
	const FName GroupTag(*(FString(DecalGroupPrefix) + Group));
	TMap<FString, ADecalActor*> Existing;
	for (TActorIterator<ADecalActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(GroupTag))
		{
			for (const FName& T : It->Tags)
			{
				if (T.ToString().StartsWith(DecalTagPrefix))
				{
					Existing.Add(T.ToString(), *It);
				}
			}
		}
	}
	int32 Placed = 0, Moved = 0;
	TSet<FString> Seen;
	for (const TSharedPtr<FJsonValue>& V : *Decals)
	{
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (!V->TryGetObject(O))
		{
			continue;
		}
		const FString Key = FString(DecalTagPrefix) + (*O)->GetStringField(TEXT("key"));
		Seen.Add(Key);
		const FVector Location = VecOf(*O, TEXT("location"), FVector::ZeroVector);
		const FVector X = VecOf(*O, TEXT("xAxis"), FVector::ForwardVector).GetSafeNormal();
		const FVector Z = VecOf(*O, TEXT("zAxis"), FVector::UpVector).GetSafeNormal();
		const FRotator Rotation = FRotationMatrix::MakeFromXZ(X, Z).Rotator();
		ADecalActor* Actor = Existing.FindRef(Key);
		if (!Actor)
		{
			FActorSpawnParameters SP;
			Actor = World->SpawnActor<ADecalActor>(Location, Rotation, SP);
			if (!Actor)
			{
				Warnings.Add(TEXT("could not spawn a decal actor"));
				continue;
			}
			Actor->Tags.Add(GroupTag);
			Actor->Tags.Add(FName(*Key));
			Actor->SetFolderPath(FName(*(TEXT("Lila/Contacts/") + Group)));
			++Placed;
		}
		else
		{
			Actor->Modify();
			Actor->SetActorLocationAndRotation(Location, Rotation);
			++Moved;
		}
		Actor->GetDecal()->DecalSize = VecOf(*O, TEXT("size"), FVector(20.0, 100.0, 20.0));
		Actor->SetDecalMaterial(Material);
	}
	int32 Removed = 0;
	for (const TPair<FString, ADecalActor*>& E : Existing)
	{
		if (!Seen.Contains(E.Key))
		{
			E.Value->Destroy();
			++Removed;
		}
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("placed"), Placed);
	Out->SetNumberField(TEXT("moved"), Moved);
	Out->SetNumberField(TEXT("removed"), Removed);
	TArray<TSharedPtr<FJsonValue>> W;
	for (const FString& S : Warnings)
	{
		W.Add(Str(S));
	}
	Out->SetArrayField(TEXT("warnings"), W);
	return Obj(Out);
}
} // namespace

void LilaLink::AddSeamMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("seamMaterials"), LilaLink::Sync(&SeamMaterials));
	Methods.Add(TEXT("seamDecals"), LilaLink::Sync(&SeamDecals));
}
