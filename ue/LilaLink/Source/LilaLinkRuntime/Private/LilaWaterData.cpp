#include "LilaWaterData.h"

#include "GameFramework/Actor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

FLilaWaterPoint ULilaWaterData::GetPointAtInputKey(float InputKey) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaWaterData::GetPointAtInputKey);

	if (Points.IsEmpty())
	{
		return FLilaWaterPoint();
	}
	const float Key = FMath::Clamp(InputKey, 0.0f, float(Points.Num() - 1));
	const int32 A = FMath::FloorToInt32(Key);
	const int32 B = FMath::Min(A + 1, Points.Num() - 1);
	const float T = Key - float(A);
	const FLilaWaterPoint& P = Points[A];
	const FLilaWaterPoint& Q = Points[B];
	FLilaWaterPoint Out;
	Out.Width = FMath::Lerp(P.Width, Q.Width, T);
	Out.Depth = FMath::Lerp(P.Depth, Q.Depth, T);
	Out.WaterLevel = FMath::Lerp(P.WaterLevel, Q.WaterLevel, T);
	Out.BedHeight = FMath::Lerp(P.BedHeight, Q.BedHeight, T);
	Out.FlowSpeed = FMath::Lerp(P.FlowSpeed, Q.FlowSpeed, T);
	Out.Discharge = FMath::Lerp(P.Discharge, Q.Discharge, T);
	Out.FlowDirection = FMath::Lerp(P.FlowDirection, Q.FlowDirection, T).GetSafeNormal(UE_SMALL_NUMBER, P.FlowDirection);
	Out.Distance = FMath::Lerp(P.Distance, Q.Distance, T);
	return Out;
}

double ULilaWaterData::GetNumber(FName Name, double Fallback) const
{
	const FString* Value = Values.Find(Name);
	return Value && Value->IsNumeric() ? FCString::Atod(**Value) : Fallback;
}

ULilaWaterData* ULilaWaterData::FindOn(const AActor* Actor)
{
	return Actor ? Actor->FindComponentByClass<ULilaWaterData>() : nullptr;
}
