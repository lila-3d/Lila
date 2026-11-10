#include "LilaSoftNetData.h"

#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_EDITOR
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(LilaSoftNetData)

namespace LilaSoftNetBind
{
FString SoftNetReport(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	FJsonSerializer::Serialize(Obj, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
	return Out;
}

FString FailSoftNet(const FString& Error)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetArrayField(TEXT("errors"), {MakeShared<FJsonValueString>(Error)});
	return SoftNetReport(Obj);
}

/** A point from Lila: where it is at bind, its influence entries and each entry's gradient. */
struct FNetPoint
{
	FVector Position;
	TArray<FVector2f> Entries;
	TArray<FVector3f> Gradients;
};

/** The barycentric weights of the point on triangle (A, B, C) nearest P. */
FVector BaryNearest(const FVector& P, const FVector& A, const FVector& B, const FVector& C)
{
	const FVector AB = B - A, AC = C - A, AP = P - A;
	const double D1 = AB | AP, D2 = AC | AP;
	if (D1 <= 0.0 && D2 <= 0.0)
	{
		return FVector(1, 0, 0);
	}
	const FVector BP = P - B;
	const double D3 = AB | BP, D4 = AC | BP;
	if (D3 >= 0.0 && D4 <= D3)
	{
		return FVector(0, 1, 0);
	}
	const double VC = D1 * D4 - D3 * D2;
	if (VC <= 0.0 && D1 >= 0.0 && D3 <= 0.0)
	{
		const double V = D1 / (D1 - D3);
		return FVector(1 - V, V, 0);
	}
	const FVector CP = P - C;
	const double D5 = AB | CP, D6 = AC | CP;
	if (D6 >= 0.0 && D5 <= D6)
	{
		return FVector(0, 0, 1);
	}
	const double VB = D5 * D2 - D1 * D6;
	if (VB <= 0.0 && D2 >= 0.0 && D6 <= 0.0)
	{
		const double W = D2 / (D2 - D6);
		return FVector(1 - W, 0, W);
	}
	const double VA = D3 * D6 - D5 * D4;
	if (VA <= 0.0 && (D4 - D3) >= 0.0 && (D5 - D6) >= 0.0)
	{
		const double W = (D4 - D3) / ((D4 - D3) + (D5 - D6));
		return FVector(0, 1 - W, W);
	}
	const double Denom = 1.0 / (VA + VB + VC);
	const double V = VB * Denom, W = VC * Denom;
	return FVector(1 - V - W, V, W);
}

/** Points hashed by position: the point at a vertex, or the nearest one at any distance. */
struct FNetGrid
{
	double Cell = 0.05;
	int32 MaxRing = 0;
	TMap<FIntVector, TArray<int32>> Cells;

	FIntVector KeyOf(const FVector& P) const { return FIntVector(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell), FMath::FloorToInt(P.Z / Cell)); }

	int32 Nearest(const TArray<FNetPoint>& Points, const FVector& P, const double Within, const int32 Rings) const
	{
		const FIntVector K = KeyOf(P);
		int32 Best = INDEX_NONE;
		double BestD = Within;
		for (int32 R = 0; R <= Rings; ++R)
		{
			if (Best != INDEX_NONE && double(R - 1) * Cell > BestD)
			{
				break;
			}
			for (int32 X = -R; X <= R; ++X)
			{
				for (int32 Y = -R; Y <= R; ++Y)
				{
					for (int32 Z = -R; Z <= R; ++Z)
					{
						if (FMath::Max3(FMath::Abs(X), FMath::Abs(Y), FMath::Abs(Z)) != R)
						{
							continue;
						}
						if (const TArray<int32>* Found = Cells.Find(K + FIntVector(X, Y, Z)))
						{
							for (const int32 I : *Found)
							{
								if (const double D = (Points[I].Position - P).Size(); D <= BestD)
								{
									BestD = D;
									Best = I;
								}
							}
						}
					}
				}
			}
		}
		return Best;
	}
};
} // namespace LilaSoftNetBind

FString ULilaSoftNetLibrary::BindSoftNet(USkeletalMesh* Mesh, const FString& Json)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaSoftNetLibrary::BindSoftNet);

#if WITH_EDITOR
	using namespace LilaSoftNetBind;
	if (!Mesh)
	{
		return FailSoftNet(TEXT("no skeletal mesh"));
	}
	TSharedPtr<FJsonObject> P;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), P) || !P.IsValid())
	{
		return FailSoftNet(TEXT("the payload isn't JSON"));
	}
	FSkeletalMeshModel* Model = Mesh->GetImportedModel();
	if (!Model || Model->LODModels.Num() == 0)
	{
		return FailSoftNet(TEXT("the mesh has no imported geometry"));
	}
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	ULilaSoftNetData* Data = NewObject<ULilaSoftNetData>(Mesh);
	auto Need = [&Ref, Mesh](const FString& Bone, FString& OutError) {
		if (Ref.FindRawBoneIndex(FName(*Bone)) == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("%s has no bone named %s: export the skeleton with deformOnly off so the Soft Net joints go too"), *Mesh->GetName(), *Bone);
			return false;
		}
		return true;
	};
	FString Error;
	for (const TSharedPtr<FJsonValue>& V : P->GetArrayField(TEXT("knots")))
	{
		const TSharedPtr<FJsonObject> K = V->AsObject();
		FLilaSoftNetKnot& Knot = Data->Knots.AddDefaulted_GetRef();
		if (!Need(K->GetStringField(TEXT("frame")), Error) || !Need(K->GetStringField(TEXT("control")), Error))
		{
			return FailSoftNet(Error);
		}
		Knot.Frame = FName(*K->GetStringField(TEXT("frame")));
		Knot.Control = FName(*K->GetStringField(TEXT("control")));
	}
	for (const TSharedPtr<FJsonValue>& V : P->GetArrayField(TEXT("tangents")))
	{
		const TSharedPtr<FJsonObject> T = V->AsObject();
		FLilaSoftNetTangent& Tangent = Data->Tangents.AddDefaulted_GetRef();
		if (!Need(T->GetStringField(TEXT("bone")), Error) || !Need(T->GetStringField(TEXT("rest")), Error))
		{
			return FailSoftNet(Error);
		}
		Tangent.Bone = FName(*T->GetStringField(TEXT("bone")));
		Tangent.Rest = FName(*T->GetStringField(TEXT("rest")));
	}
	const uint32 Dofs = uint32(Data->Knots.Num() + Data->Tangents.Num());

	TArray<FNetPoint> Points;
	FNetGrid Exact;
	FBox Bounds(ForceInit);
	for (const TSharedPtr<FJsonValue>& V : P->GetArrayField(TEXT("points")))
	{
		const TSharedPtr<FJsonObject> Obj = V->AsObject();
		const TArray<TSharedPtr<FJsonValue>>& Pos = Obj->GetArrayField(TEXT("p"));
		FNetPoint& Pt = Points.AddDefaulted_GetRef();
		Pt.Position = Pos.Num() == 3 ? FVector(Pos[0]->AsNumber(), Pos[1]->AsNumber(), Pos[2]->AsNumber()) : FVector::ZeroVector;
		for (const TSharedPtr<FJsonValue>& E : Obj->GetArrayField(TEXT("i")))
		{
			// [dof, weight] or [dof, weight, gradient x, y, z].
			const TArray<TSharedPtr<FJsonValue>>& Entry = E->AsArray();
			if ((Entry.Num() == 2 || Entry.Num() == 5) && uint32(Entry[0]->AsNumber()) < Dofs)
			{
				Pt.Entries.Add(FVector2f(FMath::AsFloat(uint32(Entry[0]->AsNumber())), float(Entry[1]->AsNumber())));
				Pt.Gradients.Add(Entry.Num() == 5 ? FVector3f(float(Entry[2]->AsNumber()), float(Entry[3]->AsNumber()), float(Entry[4]->AsNumber())) : FVector3f::ZeroVector);
			}
		}
		Exact.Cells.FindOrAdd(Exact.KeyOf(Pt.Position)).Add(Points.Num() - 1);
		Bounds += Pt.Position;
	}
	// Lila's triangles, and the triangles round each point: a vertex Lila has no point at takes the blend at the
	// nearest place on Lila's surface, found among the triangles round the nearest point and its neighbours.
	TArray<FIntVector> Tris;
	TArray<TArray<int32>> TrisOf;
	TrisOf.SetNum(Points.Num());
	if (const TArray<TSharedPtr<FJsonValue>>* Flat = nullptr; P->TryGetArrayField(TEXT("tris"), Flat))
	{
		for (int32 I = 0; I + 2 < Flat->Num(); I += 3)
		{
			const FIntVector T(int32((*Flat)[I]->AsNumber()), int32((*Flat)[I + 1]->AsNumber()), int32((*Flat)[I + 2]->AsNumber()));
			if (Points.IsValidIndex(T.X) && Points.IsValidIndex(T.Y) && Points.IsValidIndex(T.Z))
			{
				const int32 Index = Tris.Add(T);
				TrisOf[T.X].Add(Index);
				TrisOf[T.Y].Add(Index);
				TrisOf[T.Z].Add(Index);
			}
		}
	}
	FNetGrid Coarse;
	Coarse.Cell = FMath::Max(Bounds.IsValid ? Bounds.GetSize().GetMax() / 64.0 : 1.0, 0.1);
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		Coarse.Cells.FindOrAdd(Coarse.KeyOf(Points[I].Position)).Add(I);
	}
	auto Blend = [&Points, &Tris, &TrisOf, &Coarse](const FVector& Pos, TArray<FVector2f>& OutEntries, TArray<FVector3f>& OutGradients) {
		const int32 Nearest = Coarse.Nearest(Points, Pos, TNumericLimits<double>::Max(), 64);
		if (Nearest == INDEX_NONE)
		{
			return;
		}
		TSet<int32> Candidates;
		for (const int32 T : TrisOf[Nearest])
		{
			for (const int32 Corner : {Tris[T].X, Tris[T].Y, Tris[T].Z})
			{
				Candidates.Append(TrisOf[Corner]);
			}
		}
		int32 BestTri = INDEX_NONE;
		FVector BestBary(1, 0, 0);
		double BestD = TNumericLimits<double>::Max();
		for (const int32 T : Candidates)
		{
			const FVector A = Points[Tris[T].X].Position, B = Points[Tris[T].Y].Position, C = Points[Tris[T].Z].Position;
			const FVector Bary = BaryNearest(Pos, A, B, C);
			if (const double D = FVector::DistSquared(Pos, A * Bary.X + B * Bary.Y + C * Bary.Z); D < BestD)
			{
				BestD = D;
				BestTri = T;
				BestBary = Bary;
			}
		}
		if (BestTri == INDEX_NONE)
		{
			OutEntries.Append(Points[Nearest].Entries);
			OutGradients.Append(Points[Nearest].Gradients);
			return;
		}
		TMap<uint32, TPair<float, FVector3f>> Merged;
		const int32 Corners[3] = {Tris[BestTri].X, Tris[BestTri].Y, Tris[BestTri].Z};
		for (int32 K = 0; K < 3; ++K)
		{
			const FNetPoint& Pt = Points[Corners[K]];
			for (int32 E = 0; E < Pt.Entries.Num(); ++E)
			{
				TPair<float, FVector3f>& M = Merged.FindOrAdd(FMath::AsUInt(Pt.Entries[E].X), {0.0f, FVector3f::ZeroVector});
				M.Key += float(BestBary[K]) * Pt.Entries[E].Y;
				M.Value += float(BestBary[K]) * Pt.Gradients[E];
			}
		}
		for (const TPair<uint32, TPair<float, FVector3f>>& M : Merged)
		{
			OutEntries.Add(FVector2f(FMath::AsFloat(M.Key), M.Value.Key));
			OutGradients.Add(M.Value.Value);
		}
	};

	TArray<TSharedPtr<FJsonValue>> Warnings, LodReports;
	int32 Unmatched0 = 0, Vertices0 = 0;
	for (int32 L = 0; L < Model->LODModels.Num(); ++L)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaSoftNetLibrary::BindLod);

		TArray<FSoftSkinVertex> Vertices;
		Model->LODModels[L].GetVertices(Vertices);
		FLilaSoftNetLod& Out = Data->Lods.AddDefaulted_GetRef();
		Out.VertexCount = Vertices.Num();
		Out.Offsets.Reserve(Vertices.Num() + 1);
		Out.Offsets.Add(0);
		int32 Unmatched = 0;
		for (const FSoftSkinVertex& V : Vertices)
		{
			const FVector Pos(V.Position);
			if (const int32 Match = Exact.Nearest(Points, Pos, 0.05, 1); Match != INDEX_NONE)
			{
				Out.Entries.Append(Points[Match].Entries);
				Out.Gradients.Append(Points[Match].Gradients);
			}
			else
			{
				++Unmatched;
				Blend(Pos, Out.Entries, Out.Gradients);
			}
			Out.Offsets.Add(uint32(Out.Entries.Num()));
		}
		TSharedRef<FJsonObject> Lod = MakeShared<FJsonObject>();
		Lod->SetNumberField(TEXT("vertices"), Vertices.Num());
		Lod->SetNumberField(TEXT("matched"), Vertices.Num() - Unmatched);
		Lod->SetNumberField(TEXT("entries"), Out.Entries.Num());
		LodReports.Add(MakeShared<FJsonValueObject>(Lod));
		if (L == 0)
		{
			Unmatched0 = Unmatched;
			Vertices0 = Vertices.Num();
		}
	}
	if (Unmatched0 > 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("%d of %d vertices have no Lila point at their position, so they take the influence at the nearest place on Lila's surface"), Unmatched0, Vertices0)));
	}
	if (ULilaSoftNetData* Old = Mesh->GetAssetUserData<ULilaSoftNetData>())
	{
		Data->Revision = Old->Revision + 1;
		Mesh->RemoveUserDataOfClass(ULilaSoftNetData::StaticClass());
	}
	Mesh->AddAssetUserData(Data);
	Mesh->MarkPackageDirty();

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("vertices"), Vertices0);
	Obj->SetNumberField(TEXT("matched"), Vertices0 - Unmatched0);
	Obj->SetNumberField(TEXT("knots"), Data->Knots.Num());
	Obj->SetNumberField(TEXT("tangents"), Data->Tangents.Num());
	Obj->SetArrayField(TEXT("lods"), LodReports);
	Obj->SetArrayField(TEXT("warnings"), Warnings);
	Obj->SetArrayField(TEXT("errors"), {});
	return SoftNetReport(Obj);
#else
	return LilaSoftNetBind::FailSoftNet(TEXT("Soft Nets bind in the editor"));
#endif
}
