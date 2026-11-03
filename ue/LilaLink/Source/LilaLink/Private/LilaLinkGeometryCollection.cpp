#include "LilaLinkGeometryCollection.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterials.h"
#include "LilaLinkMethods.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Chaos/Adapters/CacheAdapter.h"
#include "Chaos/CacheCollection.h"
#include "Chaos/CacheEvents.h"
#include "Chaos/CacheManagerActor.h"
#include "Chaos/ChaosCache.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionActor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "LilaLinkGeometryCollection"

namespace
{
const TCHAR* MethodName = TEXT("importGeometryCollection");

/** Cache event track names the geometry collection cache adapter reads (FEnableStateEvent::EventName and FBreakingEvent::EventName). */
const FName EnableTrackName(TEXT("GC_Enable"));
const FName BreakingTrackName(TEXT("GC_Breaking"));

struct FPiece
{
	FString Name;
	FVector Position = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	int32 VertexStart = 0;
	int32 VertexCount = 0;
	int32 TriangleStart = 0;
	int32 TriangleCount = 0;
	int32 ReleaseFrame = INDEX_NONE;
};

struct FBreak
{
	int32 Frame = 0;
	FVector Position = FVector::ZeroVector;
	int32 PieceA = INDEX_NONE;
	int32 PieceB = INDEX_NONE;
	/** N s through the link when it broke. */
	double Impulse = 0.0;
	/** PieceA's velocity (cm/s) and mass (kg) as it broke free. */
	FVector Velocity = FVector::ZeroVector;
	double Mass = 0.0;
};

struct FBufferRef
{
	int64 Offset = 0;
	int64 Count = 0;
};

struct FBake
{
	FString Name;
	double Fps = 24.0;
	int32 StartFrame = 1;
	int32 FrameCount = 0;
	TArray<FString> Materials;
	TArray<FPiece> Pieces;
	TArray<FBreak> Breaks;

	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UVs;
	TArray<int32> Indices;
	TArray<int32> MaterialIds;
	TArray<uint8> Internal;
	/** Frame-major, FrameCount * Pieces.Num() entries of position xyz and rotation xyzw. */
	TArray<float> Transforms;

	FTransform PieceTransform(int32 Frame, int32 Piece) const
	{
		const float* T = &Transforms[(int64(Frame) * Pieces.Num() + Piece) * 7];
		FQuat Q(T[3], T[4], T[5], T[6]);
		Q.Normalize();
		return FTransform(Q, FVector(T[0], T[1], T[2]));
	}
};

struct FRequest
{
	FString File;
	FString PackagePath;
	FString AssetName;
	bool bCache = true;
	bool bPlace = false;
	bool bSave = true;
	LilaLink::FMaterialRequest Materials;
};

TSharedPtr<FJsonValue> Str(const FString& S) { return MakeShared<FJsonValueString>(S); }

TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& In)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& S : In)
	{
		Out.Add(Str(S));
	}
	return Out;
}

FVector VectorField(const TSharedPtr<FJsonObject>& O, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* V = nullptr;
	if (O->TryGetArrayField(Field, V) && V->Num() >= 3)
	{
		return FVector((*V)[0]->AsNumber(), (*V)[1]->AsNumber(), (*V)[2]->AsNumber());
	}
	return FVector::ZeroVector;
}

int32 IntField(const TSharedPtr<FJsonObject>& O, const TCHAR* Field, int32 Default)
{
	int32 V = Default;
	O->TryGetNumberField(Field, V);
	return V;
}

bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FRequest& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::ParseRequest);

	if (!Params->TryGetStringField(TEXT("file"), Out.File) || !FPaths::FileExists(Out.File))
	{
		OutError = FString::Printf(TEXT("%s: give file, the absolute path of a .lilagc.json (got '%s')"), MethodName, *Out.File);
		return false;
	}
	FString ContentPath;
	if (!Params->TryGetStringField(TEXT("contentPath"), ContentPath) || ContentPath.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s: give contentPath, like /Game/Destruction/GC_Wall"), MethodName);
		return false;
	}
	ContentPath = FPackageName::ObjectPathToPackageName(ContentPath);
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(ContentPath, false, &Reason))
	{
		OutError = FString::Printf(TEXT("%s: bad contentPath %s: %s"), MethodName, *ContentPath, *Reason.ToString());
		return false;
	}
	Out.PackagePath = FPackageName::GetLongPackagePath(ContentPath);
	Out.AssetName = FPackageName::GetShortName(ContentPath);
	Params->TryGetBoolField(TEXT("cache"), Out.bCache);
	Params->TryGetBoolField(TEXT("place"), Out.bPlace);
	Params->TryGetBoolField(TEXT("save"), Out.bSave);

	const TSharedPtr<FJsonObject>* OptionsPtr = nullptr;
	const TSharedPtr<FJsonObject> Options = Params->TryGetObjectField(TEXT("options"), OptionsPtr) ? *OptionsPtr : MakeShared<FJsonObject>();
	if (!LilaLink::ParseMaterialRequest(Params, Options, Out.PackagePath, Out.Materials, OutError))
	{
		return false;
	}
	// Without Lila's material list there is nothing to build a material from: match existing ones by name only.
	if (!Out.Materials.bManaged)
	{
		Out.Materials.bCreate = false;
	}
	return true;
}

/** Copies Count * Components values of T from the binary, checking the range. */
template <class T> bool ReadBuffer(const TArray<uint8>& Bin, const TSharedPtr<FJsonObject>& Buffers, const TCHAR* Name, int32 Components, int64 Expected,
	TArray<T>& Out, FString& OutError)
{
	const TSharedPtr<FJsonObject>* B = nullptr;
	if (!Buffers->TryGetObjectField(Name, B))
	{
		OutError = FString::Printf(TEXT("the manifest has no buffers.%s"), Name);
		return false;
	}
	FBufferRef Ref;
	(*B)->TryGetNumberField(TEXT("offset"), Ref.Offset);
	(*B)->TryGetNumberField(TEXT("count"), Ref.Count);
	if (Ref.Count != Expected)
	{
		OutError = FString::Printf(TEXT("buffers.%s has %lld entries, expected %lld"), Name, Ref.Count, Expected);
		return false;
	}
	const int64 Values = Ref.Count * Components;
	const int64 Bytes = Values * int64(sizeof(T));
	if (Ref.Offset < 0 || Ref.Offset + Bytes > Bin.Num())
	{
		OutError = FString::Printf(TEXT("buffers.%s (%lld bytes at %lld) runs past the end of the %d byte .bin"), Name, Bytes, Ref.Offset, Bin.Num());
		return false;
	}
	Out.SetNumUninitialized(int32(Values));
	if (Bytes > 0)
	{
		FMemory::Memcpy(Out.GetData(), Bin.GetData() + Ref.Offset, Bytes);
	}
	return true;
}

/** Reads the manifest and its binary, and checks every range in them. */
bool LoadBake(const FString& File, FBake& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::LoadBake);

	FString Text;
	TSharedPtr<FJsonObject> M;
	if (!FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), M) || !M.IsValid())
	{
		OutError = FString::Printf(TEXT("%s is not JSON"), *File);
		return false;
	}
	FString Format;
	M->TryGetStringField(TEXT("format"), Format);
	if (Format != TEXT("lila-geometry-collection"))
	{
		OutError = FString::Printf(TEXT("%s is not a Lila geometry collection (format '%s')"), *File, *Format);
		return false;
	}
	const int32 Version = IntField(M, TEXT("version"), 0);
	if (Version != 1)
	{
		OutError = FString::Printf(TEXT("%s is version %d; this plugin reads version 1"), *File, Version);
		return false;
	}
	M->TryGetStringField(TEXT("name"), Out.Name);
	M->TryGetNumberField(TEXT("fps"), Out.Fps);
	Out.StartFrame = IntField(M, TEXT("startFrame"), 1);
	Out.FrameCount = FMath::Max(0, IntField(M, TEXT("frameCount"), 0));
	if (Out.Fps <= 0.0)
	{
		OutError = FString::Printf(TEXT("fps must be positive (got %g)"), Out.Fps);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (M->TryGetArrayField(TEXT("materials"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Out.Materials.Add(V->AsString());
		}
	}
	if (!M->TryGetArrayField(TEXT("pieces"), Values) || Values->IsEmpty())
	{
		OutError = TEXT("the manifest has no pieces");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *Values)
	{
		const TSharedPtr<FJsonObject> P = V->AsObject();
		if (!P.IsValid())
		{
			OutError = TEXT("a piece is not an object");
			return false;
		}
		FPiece& Piece = Out.Pieces.AddDefaulted_GetRef();
		P->TryGetStringField(TEXT("name"), Piece.Name);
		if (Piece.Name.IsEmpty())
		{
			Piece.Name = FString::Printf(TEXT("Piece_%03d"), Out.Pieces.Num() - 1);
		}
		Piece.Position = VectorField(P, TEXT("position"));
		const TArray<TSharedPtr<FJsonValue>>* R = nullptr;
		if (P->TryGetArrayField(TEXT("rotation"), R) && R->Num() >= 4)
		{
			Piece.Rotation = FQuat((*R)[0]->AsNumber(), (*R)[1]->AsNumber(), (*R)[2]->AsNumber(), (*R)[3]->AsNumber());
			Piece.Rotation.Normalize();
		}
		Piece.VertexStart = IntField(P, TEXT("vertexStart"), 0);
		Piece.VertexCount = IntField(P, TEXT("vertexCount"), 0);
		Piece.TriangleStart = IntField(P, TEXT("triangleStart"), 0);
		Piece.TriangleCount = IntField(P, TEXT("triangleCount"), 0);
		Piece.ReleaseFrame = IntField(P, TEXT("releaseFrame"), INDEX_NONE);
	}
	if (M->TryGetArrayField(TEXT("breaks"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			const TSharedPtr<FJsonObject> B = V->AsObject();
			if (!B.IsValid())
			{
				continue;
			}
			FBreak& Break = Out.Breaks.AddDefaulted_GetRef();
			Break.Frame = IntField(B, TEXT("frame"), Out.StartFrame);
			Break.Position = VectorField(B, TEXT("position"));
			Break.PieceA = IntField(B, TEXT("pieceA"), INDEX_NONE);
			Break.PieceB = IntField(B, TEXT("pieceB"), INDEX_NONE);
			B->TryGetNumberField(TEXT("impulse"), Break.Impulse);
			B->TryGetNumberField(TEXT("mass"), Break.Mass);
			Break.Velocity = VectorField(B, TEXT("velocity"));
		}
		Out.Breaks.StableSort([](const FBreak& A, const FBreak& B) { return A.Frame < B.Frame; });
	}

	FString BinName;
	M->TryGetStringField(TEXT("bin"), BinName);
	const FString BinFile = FPaths::IsRelative(BinName) ? FPaths::GetPath(File) / BinName : BinName;
	TArray<uint8> Bin;
	if (BinName.IsEmpty() || !FFileHelper::LoadFileToArray(Bin, *BinFile))
	{
		OutError = FString::Printf(TEXT("could not read the binary '%s'"), *BinFile);
		return false;
	}
	const TSharedPtr<FJsonObject>* Buffers = nullptr;
	if (!M->TryGetObjectField(TEXT("buffers"), Buffers))
	{
		OutError = TEXT("the manifest has no buffers");
		return false;
	}

	int64 Vertices = 0, Triangles = 0;
	for (const FPiece& P : Out.Pieces)
	{
		Vertices = FMath::Max<int64>(Vertices, int64(P.VertexStart) + P.VertexCount);
		Triangles = FMath::Max<int64>(Triangles, int64(P.TriangleStart) + P.TriangleCount);
	}
	// The pieces' ranges must cover the buffers exactly, so the counts the buffers declare are checked against them.
	const TSharedPtr<FJsonObject>* PositionsRef = nullptr;
	const TSharedPtr<FJsonObject>* IndicesRef = nullptr;
	int64 DeclaredVertices = Vertices, DeclaredIndices = Triangles * 3;
	if ((*Buffers)->TryGetObjectField(TEXT("positions"), PositionsRef))
	{
		(*PositionsRef)->TryGetNumberField(TEXT("count"), DeclaredVertices);
	}
	if ((*Buffers)->TryGetObjectField(TEXT("indices"), IndicesRef))
	{
		(*IndicesRef)->TryGetNumberField(TEXT("count"), DeclaredIndices);
	}
	if (DeclaredVertices < Vertices || DeclaredIndices < Triangles * 3)
	{
		OutError = FString::Printf(TEXT("the pieces reach vertex %lld and triangle %lld but the buffers hold %lld and %lld"), Vertices, Triangles,
			DeclaredVertices, DeclaredIndices / 3);
		return false;
	}
	Vertices = DeclaredVertices;
	Triangles = DeclaredIndices / 3;

	TArray<float> Floats;
	if (!ReadBuffer(Bin, *Buffers, TEXT("positions"), 3, Vertices, Floats, OutError))
	{
		return false;
	}
	Out.Positions.SetNumUninitialized(int32(Vertices));
	FMemory::Memcpy(Out.Positions.GetData(), Floats.GetData(), Vertices * sizeof(FVector3f));
	if (!ReadBuffer(Bin, *Buffers, TEXT("normals"), 3, Vertices, Floats, OutError))
	{
		return false;
	}
	Out.Normals.SetNumUninitialized(int32(Vertices));
	FMemory::Memcpy(Out.Normals.GetData(), Floats.GetData(), Vertices * sizeof(FVector3f));
	if (!ReadBuffer(Bin, *Buffers, TEXT("uvs"), 2, Vertices, Floats, OutError))
	{
		return false;
	}
	Out.UVs.SetNumUninitialized(int32(Vertices));
	FMemory::Memcpy(Out.UVs.GetData(), Floats.GetData(), Vertices * sizeof(FVector2f));
	if (!ReadBuffer(Bin, *Buffers, TEXT("indices"), 1, Triangles * 3, Out.Indices, OutError)
		|| !ReadBuffer(Bin, *Buffers, TEXT("materialIds"), 1, Triangles, Out.MaterialIds, OutError)
		|| !ReadBuffer(Bin, *Buffers, TEXT("internal"), 1, Triangles, Out.Internal, OutError)
		|| !ReadBuffer(Bin, *Buffers, TEXT("transforms"), 7, int64(Out.FrameCount) * Out.Pieces.Num(), Out.Transforms, OutError))
	{
		return false;
	}

	for (const FPiece& P : Out.Pieces)
	{
		if (P.VertexStart < 0 || P.VertexCount <= 0 || P.TriangleStart < 0 || P.TriangleCount <= 0)
		{
			OutError = FString::Printf(TEXT("piece %s has no geometry"), *P.Name);
			return false;
		}
		for (int32 K = P.TriangleStart * 3; K < (P.TriangleStart + P.TriangleCount) * 3; ++K)
		{
			if (Out.Indices[K] < 0 || Out.Indices[K] >= P.VertexCount)
			{
				OutError = FString::Printf(TEXT("piece %s has index %d outside its %d vertices"), *P.Name, Out.Indices[K], P.VertexCount);
				return false;
			}
		}
	}
	return true;
}

/**
 * Per-vertex tangents from the UVs, accumulated over each piece's triangles, then made orthonormal to the normal.
 * TangentV follows the static mesh convention: sign * cross(Normal, TangentU).
 */
void ComputeTangents(const FBake& Bake, const FPiece& P, TArray<FVector3f>& OutU, TArray<FVector3f>& OutV)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::ComputeTangents);

	TArray<FVector3f> Tan, Bit;
	Tan.Init(FVector3f::ZeroVector, P.VertexCount);
	Bit.Init(FVector3f::ZeroVector, P.VertexCount);
	for (int32 T = P.TriangleStart; T < P.TriangleStart + P.TriangleCount; ++T)
	{
		const int32 I[3] = {Bake.Indices[T * 3], Bake.Indices[T * 3 + 1], Bake.Indices[T * 3 + 2]};
		const FVector3f& P0 = Bake.Positions[P.VertexStart + I[0]];
		const FVector2f& T0 = Bake.UVs[P.VertexStart + I[0]];
		const FVector3f E1 = Bake.Positions[P.VertexStart + I[1]] - P0;
		const FVector3f E2 = Bake.Positions[P.VertexStart + I[2]] - P0;
		const FVector2f D1 = Bake.UVs[P.VertexStart + I[1]] - T0;
		const FVector2f D2 = Bake.UVs[P.VertexStart + I[2]] - T0;
		const float Det = D1.X * D2.Y - D2.X * D1.Y;
		if (FMath::Abs(Det) < UE_SMALL_NUMBER)
		{
			continue;
		}
		const float R = 1.0f / Det;
		const FVector3f FaceT = (E1 * D2.Y - E2 * D1.Y) * R;
		const FVector3f FaceB = (E2 * D1.X - E1 * D2.X) * R;
		for (int32 K : I)
		{
			Tan[K] += FaceT;
			Bit[K] += FaceB;
		}
	}
	OutU.SetNum(P.VertexCount);
	OutV.SetNum(P.VertexCount);
	for (int32 K = 0; K < P.VertexCount; ++K)
	{
		const FVector3f N = Bake.Normals[P.VertexStart + K].GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
		FVector3f U = (Tan[K] - N * FVector3f::DotProduct(N, Tan[K])).GetSafeNormal();
		if (U.IsNearlyZero())
		{
			U = FVector3f::CrossProduct(FMath::Abs(N.Z) < 0.9f ? FVector3f::UnitZ() : FVector3f::UnitX(), N).GetSafeNormal();
		}
		const FVector3f V = FVector3f::CrossProduct(N, U);
		OutU[K] = U;
		OutV[K] = FVector3f::DotProduct(V, Bit[K]) < 0.0f ? -V : V;
	}
}

/**
 * The bake as a collection: transform 0 is a root cluster at identity, transform i + 1 is piece i (rigid, at its rest
 * transform) owning geometry i, whose vertices are in that transform's space. Faces are wound for Unreal (front face
 * clockwise in its left-handed space), the opposite of the manifest's order. OutRestBounds gets each piece's bounds at rest.
 */
void BuildCollection(const FBake& Bake, int32 MaterialCount, FGeometryCollection& C, TArray<FBox>& OutRestBounds, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::BuildCollection);

	const int32 NumPieces = Bake.Pieces.Num();
	int32 NumVertices = 0, NumFaces = 0;
	for (const FPiece& P : Bake.Pieces)
	{
		NumVertices += P.VertexCount;
		NumFaces += P.TriangleCount;
	}

	C.AddElements(NumPieces + 1, FGeometryCollection::TransformGroup);
	C.SetNumUVLayers(1);
	C.AddElements(NumVertices, FGeometryCollection::VerticesGroup);
	C.AddElements(NumFaces, FGeometryCollection::FacesGroup);
	C.AddElements(NumPieces, FGeometryCollection::GeometryGroup);
	TManagedArray<int32>& Level = C.AddAttribute<int32>(TEXT("Level"), FGeometryCollection::TransformGroup);

	C.Transform[0] = FTransform3f::Identity;
	C.BoneName[0] = Bake.Name;
	C.Parent[0] = FGeometryCollection::Invalid;
	C.SimulationType[0] = FGeometryCollection::ESimulationTypes::FST_Clustered;
	C.TransformToGeometryIndex[0] = INDEX_NONE;
	C.BoneColor[0] = FLinearColor::White;
	Level[0] = 0;

	OutRestBounds.SetNum(NumPieces);
	int32 BadMaterial = 0;
	int32 Vertex = 0, Face = 0;
	TArray<FVector3f> TangentU, TangentV;
	for (int32 Index = 0; Index < NumPieces; ++Index)
	{
		const FPiece& P = Bake.Pieces[Index];
		const int32 Bone = Index + 1;
		C.Transform[Bone] = FTransform3f(FQuat4f(P.Rotation), FVector3f(P.Position));
		C.BoneName[Bone] = P.Name;
		C.Parent[Bone] = 0;
		C.Children[0].Add(Bone);
		C.SimulationType[Bone] = FGeometryCollection::ESimulationTypes::FST_Rigid;
		C.TransformToGeometryIndex[Bone] = Index;
		C.BoneColor[Bone] = FLinearColor::White;
		Level[Bone] = 1;

		ComputeTangents(Bake, P, TangentU, TangentV);
		const int32 VertexStart = Vertex;
		FBox Local(ForceInit);
		for (int32 K = 0; K < P.VertexCount; ++K, ++Vertex)
		{
			const int32 Src = P.VertexStart + K;
			C.Vertex[Vertex] = Bake.Positions[Src];
			C.Normal[Vertex] = Bake.Normals[Src].GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
			C.TangentU[Vertex] = TangentU[K];
			C.TangentV[Vertex] = TangentV[K];
			C.ModifyUV(Vertex, 0) = Bake.UVs[Src];
			C.Color[Vertex] = FLinearColor::White;
			C.BoneMap[Vertex] = Bone;
			Local += FVector(Bake.Positions[Src]);
		}

		const int32 FaceStart = Face;
		for (int32 T = P.TriangleStart; T < P.TriangleStart + P.TriangleCount; ++T, ++Face)
		{
			C.Indices[Face] = FIntVector(VertexStart + Bake.Indices[T * 3], VertexStart + Bake.Indices[T * 3 + 2], VertexStart + Bake.Indices[T * 3 + 1]);
			C.Visible[Face] = true;
			int32 Material = Bake.MaterialIds[T];
			if (Material < 0 || Material >= MaterialCount)
			{
				++BadMaterial;
				Material = 0;
			}
			C.MaterialID[Face] = Material;
			C.MaterialIndex[Face] = Face;
			C.Internal[Face] = Bake.Internal[T] != 0;
		}

		C.TransformIndex[Index] = Bone;
		C.VertexStart[Index] = VertexStart;
		C.VertexCount[Index] = P.VertexCount;
		C.FaceStart[Index] = FaceStart;
		C.FaceCount[Index] = P.TriangleCount;
		C.BoundingBox[Index] = Local;
		GeometryCollection::ComputeInnerAndOuterRadiiFromGeometryVertices(C.Vertex, VertexStart, P.VertexCount, C.InnerRadius[Index], C.OuterRadius[Index]);
		OutRestBounds[Index] = Local.TransformBy(FTransform(P.Rotation, P.Position));
	}
	if (BadMaterial > 0)
	{
		OutWarnings.Add(FString::Printf(TEXT("%d face(s) had a material id outside the %d materials and use the first"), BadMaterial, MaterialCount));
	}
	C.ReindexMaterials();
}

/** The asset of type T at PackagePath/Name, made when missing; null with an error when something else is there. */
template <class T> T* FindOrMakeAsset(const FString& PackagePath, const FString& Name, bool& bOutCreated, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::FindOrMakeAsset);

	const FString PackageName = PackagePath / Name;
	bOutCreated = false;
	if (UObject* Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *(PackageName + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		if (T* Typed = Cast<T>(Existing))
		{
			Typed->Modify();
			return Typed;
		}
		OutErrors.Add(FString::Printf(TEXT("%s is a %s, not a %s"), *PackageName, *Existing->GetClass()->GetName(), *T::StaticClass()->GetName()));
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackageName);
	T* Asset = NewObject<T>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	bOutCreated = true;
	return Asset;
}

/** Fills the collection asset from the bake and rebuilds its simulation and render data. */
UGeometryCollection* WriteCollection(const FRequest& R, const FBake& Bake, TArray<FBox>& OutRestBounds, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings, TArray<FString>& OutErrors, TArray<LilaLink::FSlotResult>& OutSlots)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::WriteCollection);

	bool bCreated = false;
	UGeometryCollection* Asset = FindOrMakeAsset<UGeometryCollection>(R.PackagePath, R.AssetName, bCreated, OutErrors);
	if (!Asset)
	{
		return nullptr;
	}

	TArray<FName> Slots;
	for (const FString& M : Bake.Materials)
	{
		Slots.Add(FName(*M));
	}
	if (Slots.IsEmpty())
	{
		Slots.Add(LilaLink::DefaultSlotName);
	}
	TArray<UMaterialInterface*> Materials = LilaLink::ResolveSlotMaterials(R.Materials, Asset->GetPathName(), Slots, OutCreated, OutWarnings, OutSlots);
	for (UMaterialInterface*& M : Materials)
	{
		if (!M)
		{
			M = UMaterial::GetDefaultMaterial(MD_Surface);
		}
	}

	FGeometryCollection Built;
	BuildCollection(Bake, Materials.Num(), Built, OutRestBounds, OutWarnings);

	// Interior faces are flagged per face (Internal), so the materials are not laid out as exterior/interior pairs.
	Asset->ResetFrom(Built, Materials, false);
	::GeometryCollection::GenerateTemporaryGuids(Asset->GetGeometryCollection().Get(), 0, true);
	Asset->InvalidateCollection();
	Asset->CreateSimulationData();
	Asset->RebuildRenderData();
	Asset->PropagateMarkDirtyToComponents();
	Asset->MarkPackageDirty();
	if (bCreated)
	{
		OutCreated.AddUnique(Asset->GetPathName());
	}
	return Asset;
}

void SetEventField(const UScriptStruct* Struct, void* Data, const TCHAR* Name, bool Value)
{
	if (const FBoolProperty* P = CastField<FBoolProperty>(Struct->FindPropertyByName(Name)))
	{
		P->SetPropertyValue_InContainer(Data, Value);
	}
}

template <class T> void SetEventField(const UScriptStruct* Struct, void* Data, const TCHAR* Name, const T& Value)
{
	const FProperty* P = Struct->FindPropertyByName(Name);
	if (P && P->GetElementSize() == sizeof(T))
	{
		*P->ContainerPtrToValuePtr<T>(Data) = Value;
	}
}

/**
 * Appends events to one of the cache's event tracks. The track and event types are not exported from ChaosCaching, so
 * the track is reached through the cache's reflected EventTracks map and the events are built from their UScriptStruct,
 * allocated as the track frees them. Fill sets an event's fields and returns its time; times must not decrease.
 */
bool AddCacheEvents(UChaosCache* Cache, FName TrackName, const TCHAR* StructName, int32 Count,
	TFunctionRef<float(int32 Index, const UScriptStruct* Struct, void* Event)> Fill)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::AddCacheEvents);

	UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *FString::Printf(TEXT("/Script/ChaosCaching.%s"), StructName));
	FMapProperty* Tracks = FindFProperty<FMapProperty>(UChaosCache::StaticClass(), TEXT("EventTracks"));
	if (!Struct || !Tracks)
	{
		return false;
	}
	FScriptMapHelper Map(Tracks, Tracks->ContainerPtrToValuePtr<void>(Cache));
	FCacheEventTrack* Track = static_cast<FCacheEventTrack*>(Map.FindOrAdd(&TrackName));
	if (!Track)
	{
		return false;
	}
	Track->Name = TrackName;
	Track->Struct = Struct;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		uint8* Event = static_cast<uint8*>(FMemory::Malloc(Struct->GetStructureSize()));
		Struct->InitializeStruct(Event);
		Track->TimeStamps.Add(Fill(Index, Struct, Event));
		Track->EventData.Add(Event);
	}
	return true;
}

/**
 * CC_<name> beside the collection, holding one cache named after the bake. Keyed in the layout the geometry collection
 * cache adapter records: track particle indices are transform indices, transforms are in the actor's space with
 * MassToLocal removed (the bone transforms themselves, since the root is at identity), a released cluster's track ends
 * on its release key with bDeactivateOnEnd, and GC_Enable events at that time break it.
 */
UChaosCacheCollection* WriteCache(const FRequest& R, const FBake& Bake, UGeometryCollection* Collection, const TArray<FBox>& RestBounds,
	TArray<FString>& OutCreated, TArray<FString>& OutWarnings, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::WriteCache);

	const FString Name = TEXT("CC_") + Bake.Name;
	bool bCreated = false;
	UChaosCacheCollection* Caches = FindOrMakeAsset<UChaosCacheCollection>(R.PackagePath, Name, bCreated, OutErrors);
	if (!Caches)
	{
		return nullptr;
	}
	UChaosCache* Cache = Caches->FindOrAddCache(FName(*Bake.Name));
	if (!Cache)
	{
		OutErrors.Add(FString::Printf(TEXT("could not add cache %s to %s"), *Bake.Name, *Caches->GetPathName()));
		return nullptr;
	}

	// BeginRecord stores the adapter's id and a spawnable copy of the component the cache plays on.
	UGeometryCollectionComponent* Probe = NewObject<UGeometryCollectionComponent>(GetTransientPackage());
	Probe->SetRestCollection(Collection);
	FGuid AdapterGuid;
	if (Chaos::FComponentCacheAdapter* Adapter = Chaos::FAdapterUtil::GetBestAdapterForClass(UGeometryCollectionComponent::StaticClass()))
	{
		AdapterGuid = Adapter->GetGuid();
	}
	FCacheUserToken Token = Cache->BeginRecord(Probe, AdapterGuid, FTransform::Identity);
	if (!Token.IsOpen())
	{
		OutErrors.Add(FString::Printf(TEXT("%s is open for playback or recording elsewhere; stop its cache manager and reimport"), *Cache->GetPathName()));
		return Caches;
	}

	const int32 NumPieces = Bake.Pieces.Num();
	const int32 Last = Bake.FrameCount - 1;
	auto TimeOf = [&Bake](int32 FrameIndex) { return float(FrameIndex / Bake.Fps); };

	// Each piece stays in the cluster until its own release; the cluster lets go of the last one and only then ends.
	TArray<int32> ReleaseOf;
	ReleaseOf.Init(INDEX_NONE, NumPieces);
	int32 LastRelease = INDEX_NONE;
	bool bAllReleased = NumPieces > 0;
	for (int32 Piece = 0; Piece < NumPieces; ++Piece)
	{
		const FPiece& P = Bake.Pieces[Piece];
		if (P.ReleaseFrame < 0)
		{
			bAllReleased = false;
			continue;
		}
		ReleaseOf[Piece] = FMath::Clamp(P.ReleaseFrame - Bake.StartFrame, 0, Last);
		LastRelease = FMath::Max(LastRelease, ReleaseOf[Piece]);
	}

	auto AddTrack = [Cache](int32 Particle) -> FParticleTransformTrack& {
		Cache->TrackToParticle.Add(Particle);
		return Cache->ParticleTracks.AddDefaulted_GetRef().TransformData;
	};
	auto AddKey = [](FParticleTransformTrack& Track, float Time, const FTransform& T) {
		if (Track.KeyTimestamps.IsEmpty())
		{
			Track.BeginOffset = Time;
		}
		Track.KeyTimestamps.Add(Time);
		Track.RawTransformTrack.PosKeys.Add(FVector3f(T.GetTranslation()));
		Track.RawTransformTrack.RotKeys.Add(FQuat4f(T.GetRotation()));
		Track.RawTransformTrack.ScaleKeys.Add(FVector3f::OneVector);
	};

	Cache->ParticleTracks.Reserve(NumPieces + 1);
	Cache->TrackToParticle.Reserve(NumPieces + 1);

	// The root cluster holds the unreleased pieces at rest; it ends once its last piece has gone, or plays to the end when
	// some never leave.
	FParticleTransformTrack& Root = AddTrack(0);
	const int32 RootLast = bAllReleased ? LastRelease : Last;
	for (int32 F = 0; F <= RootLast; ++F)
	{
		AddKey(Root, TimeOf(F), FTransform::Identity);
	}
	Root.bDeactivateOnEnd = bAllReleased;

	TArray<int32> Released;
	for (int32 Piece = 0; Piece < NumPieces; ++Piece)
	{
		if (ReleaseOf[Piece] == INDEX_NONE)
		{
			continue;
		}
		Released.Add(Piece);
		FParticleTransformTrack& Track = AddTrack(Piece + 1);
		for (int32 F = ReleaseOf[Piece]; F <= Last; ++F)
		{
			AddKey(Track, TimeOf(F), Bake.PieceTransform(F, Piece));
		}
	}
	// Events are read in time order.
	Released.StableSort([&ReleaseOf](int32 A, int32 B) { return ReleaseOf[A] < ReleaseOf[B]; });
	Cache->EndRecord(Token);
	Cache->RecordedDuration = TimeOf(Last);
	Cache->NumRecordedFrames = uint32(Bake.FrameCount);

	if (!Released.IsEmpty())
	{
		// Playback reads events strictly after the previous tick's time, so none may sit exactly on 0.
		const bool bEnable = AddCacheEvents(Cache, EnableTrackName, TEXT("EnableStateEvent"), Released.Num(),
			[&Released, &ReleaseOf, &TimeOf](int32 Index, const UScriptStruct* Struct, void* Event) {
				const int32 Piece = Released[Index];
				SetEventField(Struct, Event, TEXT("Index"), int32(Piece + 1));
				SetEventField(Struct, Event, TEXT("bEnable"), true);
				return FMath::Max(TimeOf(ReleaseOf[Piece]), 1e-3f);
			});
		if (!bEnable)
		{
			OutErrors.Add(TEXT("could not write the cache's GC_Enable events: the pieces will play as one unbroken cluster"));
		}
	}
	if (!Bake.Breaks.IsEmpty())
	{
		const bool bBreaks = AddCacheEvents(Cache, BreakingTrackName, TEXT("BreakingEvent"), Bake.Breaks.Num(),
			[&Bake, &RestBounds, &TimeOf](int32 Index, const UScriptStruct* Struct, void* Event) {
				const FBreak& B = Bake.Breaks[Index];
				const int32 Piece = Bake.Pieces.IsValidIndex(B.PieceA) ? B.PieceA : Bake.Pieces.IsValidIndex(B.PieceB) ? B.PieceB : INDEX_NONE;
				const FBox Bounds = Piece != INDEX_NONE ? RestBounds[Piece] : FBox(B.Position, B.Position);
				SetEventField(Struct, Event, TEXT("Index"), int32(Piece + 1));
				SetEventField(Struct, Event, TEXT("Location"), B.Position);
				SetEventField(Struct, Event, TEXT("Orientation"), FQuat::Identity);
				SetEventField(Struct, Event, TEXT("BoundingBoxMin"), Bounds.Min);
				SetEventField(Struct, Event, TEXT("BoundingBoxMax"), Bounds.Max);
				SetEventField(Struct, Event, TEXT("Velocity"), B.Velocity);
				SetEventField(Struct, Event, TEXT("Mass"), float(B.Mass));
				return FMath::Max(TimeOf(B.Frame - Bake.StartFrame), 1e-3f);
			});
		if (!bBreaks)
		{
			OutWarnings.Add(TEXT("could not write the cache's GC_Breaking events; breaks are left out"));
		}
		// Breaking events have no impulse field: the total N s broken on each frame rides along as a stepped curve.
		FRichCurve& Impulse = Cache->CurveData.FindOrAdd(FName(TEXT("BreakImpulse")));
		Impulse.Reset();
		TMap<int32, double> PerFrame;
		for (const FBreak& B : Bake.Breaks)
		{
			PerFrame.FindOrAdd(B.Frame - Bake.StartFrame) += B.Impulse;
		}
		PerFrame.KeySort(TLess<int32>());
		for (const TPair<int32, double>& K : PerFrame)
		{
			Impulse.SetKeyInterpMode(Impulse.AddKey(TimeOf(K.Key), float(K.Value)), RCIM_Constant);
			if (K.Key + 1 <= Last && !PerFrame.Contains(K.Key + 1))
			{
				Impulse.SetKeyInterpMode(Impulse.AddKey(TimeOf(K.Key + 1), 0.0f), RCIM_Constant);
			}
		}
	}

	Cache->MarkPackageDirty();
	Caches->MarkPackageDirty();
	if (bCreated)
	{
		OutCreated.AddUnique(Caches->GetPathName());
	}
	return Caches;
}

/** A Geometry Collection actor at the origin and, with a cache, a Chaos Cache Manager playing it on that actor's component. */
TArray<FString> Place(const FBake& Bake, UGeometryCollection* Collection, UChaosCacheCollection* Caches, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::Place);

	TArray<FString> Names;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutErrors.Add(TEXT("place: no editor world"));
		return Names;
	}
	FScopedTransaction Transaction(LOCTEXT("PlaceGeometryCollection", "Lila: Place Geometry Collection"));
	FActorSpawnParameters Params;
	Params.OverrideLevel = World->GetCurrentLevel();
	AGeometryCollectionActor* Actor = World->SpawnActor<AGeometryCollectionActor>(AGeometryCollectionActor::StaticClass(), FTransform::Identity, Params);
	if (!Actor || !Actor->GetGeometryCollectionComponent())
	{
		OutErrors.Add(TEXT("place: could not spawn a Geometry Collection actor"));
		return Names;
	}
	UGeometryCollectionComponent* Component = Actor->GetGeometryCollectionComponent();
	Component->SetRestCollection(Collection);
	if (!Collection->PhysicsMaterial && GEngine)
	{
		Component->SetPhysMaterialOverride(GEngine->DefaultDestructiblePhysMaterial);
	}
	Component->bNotifyBreaks = !Bake.Breaks.IsEmpty();
	Actor->SetActorLabel(Collection->GetName());
	Names.Add(Actor->GetName());

	if (Caches)
	{
		AChaosCacheManager* Manager = World->SpawnActor<AChaosCacheManager>(AChaosCacheManager::StaticClass(), FTransform::Identity, Params);
		if (!Manager)
		{
			OutErrors.Add(TEXT("place: could not spawn a Chaos Cache Manager"));
			return Names;
		}
		Manager->CacheCollection = Caches;
		Manager->FindOrAddObservedComponent(Component, FName(*Bake.Name), false);
		// After the component is observed: Play mode turns its physics simulation off, without which it falls as one body
		// before the cache can take it over.
		Manager->SetCacheMode(ECacheMode::Play);
		Manager->SetStartTime(0.0f);
		Manager->SetActorLabel(TEXT("CM_") + Collection->GetName());
		Names.Add(Manager->GetName());
	}
	return Names;
}

void ImportGeometryCollection(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeometryCollection::ImportGeometryCollection);

	FRequest R;
	FString Error;
	if (!ParseRequest(Params, R, Error))
	{
		Call->Reply(nullptr, Error);
		return;
	}
	FBake Bake;
	Call->Progress(TEXT("reading the bake"));
	if (!LoadBake(R.File, Bake, Error))
	{
		Call->Reply(nullptr, FString::Printf(TEXT("%s: %s"), MethodName, *Error));
		return;
	}
	// The name becomes the cache's object name and CC_<name>.
	Bake.Name = ObjectTools::SanitizeObjectName(Bake.Name);
	if (Bake.Name.IsEmpty())
	{
		Bake.Name = R.AssetName;
	}

	TArray<FString> Assets, Created, Warnings, Errors, Actors;
	TArray<LilaLink::FSlotResult> Slots;
	TArray<FBox> RestBounds;
	Call->Progress(TEXT("building the geometry collection"));
	UGeometryCollection* Collection = WriteCollection(R, Bake, RestBounds, Created, Warnings, Errors, Slots);
	UChaosCacheCollection* Caches = nullptr;
	if (Collection)
	{
		Assets.Add(Collection->GetPathName());
		if (R.bCache && Bake.FrameCount > 0)
		{
			Call->Progress(TEXT("writing the Chaos cache"));
			Caches = WriteCache(R, Bake, Collection, RestBounds, Created, Warnings, Errors);
			if (Caches)
			{
				Assets.Add(Caches->GetPathName());
			}
		}
		else if (R.bCache)
		{
			Warnings.Add(TEXT("the bake has no frames, so no cache was written"));
		}
		if (R.bPlace)
		{
			Actors = Place(Bake, Collection, Caches, Errors);
		}
	}

	if (!Assets.IsEmpty() && R.bSave)
	{
		Call->Progress(TEXT("saving"));
		TArray<FString> ToSave = Assets;
		for (const FString& C : Created)
		{
			ToSave.AddUnique(C);
		}
		LilaLink::SaveAssetPaths(ToSave, Errors);
	}
	for (const FString& W : Warnings)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("%s %s: %s"), MethodName, *R.AssetName, *W);
	}
	for (const FString& E : Errors)
	{
		UE_LOG(LogLilaLink, Error, TEXT("%s %s: %s"), MethodName, *R.AssetName, *E);
	}
	UE_LOG(LogLilaLink, Log, TEXT("%s %s/%s: %d piece(s), %d frame(s) at %g fps, %d break(s)%s"), MethodName, *R.PackagePath, *R.AssetName, Bake.Pieces.Num(),
		Bake.FrameCount, Bake.Fps, Bake.Breaks.Num(), Caches ? TEXT(", cached") : TEXT(""));

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("assets"), Strings(Assets));
	Out->SetArrayField(TEXT("created"), Strings(Created));
	Out->SetArrayField(TEXT("warnings"), Strings(Warnings));
	Out->SetArrayField(TEXT("errors"), Strings(Errors));
	if (Caches)
	{
		Out->SetStringField(TEXT("cache"), Bake.Name);
	}
	if (R.bPlace)
	{
		Out->SetArrayField(TEXT("actors"), Strings(Actors));
	}
	TArray<TSharedPtr<FJsonValue>> SlotValues;
	for (const LilaLink::FSlotResult& S : Slots)
	{
		TSharedRef<FJsonObject> Slot = MakeShared<FJsonObject>();
		Slot->SetStringField(TEXT("asset"), S.Asset);
		Slot->SetStringField(TEXT("slot"), S.Slot.ToString());
		Slot->SetStringField(TEXT("material"), S.Material);
		Slot->SetStringField(TEXT("how"), S.How);
		SlotValues.Add(MakeShared<FJsonValueObject>(Slot));
	}
	Out->SetArrayField(TEXT("materialSlots"), SlotValues);
	Call->Reply(MakeShared<FJsonValueObject>(Out));
}
} // namespace

void LilaLink::AddGeometryCollectionMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("importGeometryCollection"), &ImportGeometryCollection);
}

#undef LOCTEXT_NAMESPACE
