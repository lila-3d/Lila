// Lila Bend Bones: the custom kernel of a Deformer Graph that bends a mesh along its chains' curves as Lila's bend bones
// deformer does (src/deform/BendBones.cpp, deformPoint). Paste it into a Custom Compute Kernel; its inputs:
//
//   Per chain, the posed curve sampled along its length (written each frame from the chain's bones):
//     CurvePoint[i], CurveFrame[i] (a quaternion: tangent, normal, binormal as its X, Y, Z axes), CurveLength[i] (arc
//     length from the chain's start), CurveKnot[j] (the sample at each joint), with ChainSampleStart/ChainKnotStart/
//     ChainSampleCount per chain into those buffers.
//   Per vertex and influencing chain (up to two), from Lila's binding: Chain, Weight, Segment, Fraction (along the
//     segment's arc length), Offset (along the tangent, normal, binormal), RestStrand (how far along its segment's strand
//     the vertex sat at rest, which Lila works out once at bind), RestSpan (the segment's rest arc length).
//
// Volume and EvenBend are Lila's Options (both 1 by default).

float3 RotateByQuat(float4 Q, float3 V)
{
	const float3 T = 2.0 * cross(Q.xyz, V);
	return V + Q.w * T + cross(Q.xyz, T);
}

float3x3 FrameOf(float4 Q)
{
	return float3x3(RotateByQuat(Q, float3(1, 0, 0)), RotateByQuat(Q, float3(0, 1, 0)), RotateByQuat(Q, float3(0, 0, 1)));
}

/** The sample before arc length S on a chain and how far past it. */
void SampleAt(uint Start, uint Count, float S, out uint I, out float F)
{
	I = Start;
	for (uint K = Start + 1; K + 1 < Start + Count && CurveLength[K] <= S; ++K)
	{
		I = K;
	}
	const float Span = CurveLength[I + 1] - CurveLength[I];
	F = Span > 1e-6 ? saturate((S - CurveLength[I]) / Span) : 0.0;
}

float3 PointAt(uint Start, uint Count, float S)
{
	uint I;
	float F;
	SampleAt(Start, Count, S, I, F);
	return lerp(CurvePoint[I], CurvePoint[I + 1], F);
}

float4 FrameQuatAt(uint Start, uint Count, float S)
{
	uint I;
	float F;
	SampleAt(Start, Count, S, I, F);
	float4 A = CurveFrame[I], B = CurveFrame[I + 1];
	if (dot(A, B) < 0.0)
	{
		B = -B;
	}
	return normalize(lerp(A, B, F));
}

/** The arc length where the strand at Across has covered Fraction of its length over segment K. */
float StrandLength(uint Start, uint KnotStart, uint K, float2 Across, float Fraction)
{
	const uint From = Start + CurveKnot[KnotStart + K], To = Start + CurveKnot[KnotStart + K + 1];
	float Total = 0.0;
	float3 Prev = CurvePoint[From] + RotateByQuat(CurveFrame[From], float3(0, Across.x, Across.y));
	for (uint I = From + 1; I <= To; ++I)
	{
		const float3 Q = CurvePoint[I] + RotateByQuat(CurveFrame[I], float3(0, Across.x, Across.y));
		Total += length(Q - Prev);
		Prev = Q;
	}
	const float Want = saturate(Fraction) * Total;
	float Run = 0.0;
	Prev = CurvePoint[From] + RotateByQuat(CurveFrame[From], float3(0, Across.x, Across.y));
	for (uint J = From + 1; J <= To; ++J)
	{
		const float3 Q = CurvePoint[J] + RotateByQuat(CurveFrame[J], float3(0, Across.x, Across.y));
		const float D = length(Q - Prev);
		if (Run + D >= Want)
		{
			const float F = D > 1e-6 ? (Want - Run) / D : 0.0;
			return lerp(CurveLength[J - 1], CurveLength[J], F);
		}
		Run += D;
		Prev = Q;
	}
	return CurveLength[To];
}

/** Where a vertex bound to one chain goes on the posed curve. */
float3 DeformOnChain(uint Chain, uint Segment, float Fraction, float3 Offset, float RestStrand, float RestSpan, float Volume, float EvenBend)
{
	const uint Start = ChainSampleStart[Chain], Count = ChainSampleCount[Chain], KnotStart = ChainKnotStart[Chain];
	const float Pa = CurveLength[Start + CurveKnot[KnotStart + Segment]];
	const float Pb = CurveLength[Start + CurveKnot[KnotStart + Segment + 1]];
	const float Stretch = RestSpan > 1e-6 ? (Pb - Pa) / RestSpan : 1.0;
	const float Thin = lerp(1.0, rsqrt(max(Stretch, 1e-3)), saturate(Volume));
	const float2 Across = Offset.yz * Thin;

	float S = Pa + Fraction * (Pb - Pa);
	if (EvenBend > 0.0 && length(Across) > 1e-6)
	{
		S = lerp(S, StrandLength(Start, KnotStart, Segment, Across, RestStrand), saturate(EvenBend));
	}
	const float3x3 F = FrameOf(FrameQuatAt(Start, Count, S));
	float3 D = F[1] * Across.x + F[2] * Across.y;
	// Held short of the bend's centre: an offset reaching past it would fold the inside through itself.
	const float H = max(1e-3, (CurveLength[Start + Count - 1] - CurveLength[Start]) / float(Count - 1));
	const float3 BendRate = (FrameOf(FrameQuatAt(Start, Count, S + H))[0] - FrameOf(FrameQuatAt(Start, Count, S - H))[0]) / (2.0 * H);
	const float Curvature = length(BendRate);
	if (Curvature > 1e-6)
	{
		const float3 ToCentre = BendRate / Curvature;
		const float Reach = dot(D, ToCentre), Limit = 0.85 / Curvature;
		if (Reach > Limit)
		{
			D -= (Reach - Limit) * ToCentre;
		}
	}
	return PointAt(Start, Count, S) + F[0] * Offset.x + D;
}
