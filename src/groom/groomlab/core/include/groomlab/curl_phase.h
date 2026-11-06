// SPDX-License-Identifier: Apache-2.0
#pragma once

// The phase of Curl's turns (curl_op.cpp, and the GPU preview's CurlCS through shared/groom_math.h):
// how far round its curl a point is, and how fast the curl turns there. A display that places
// points by the turn reads it here.
//
// A curl turns at the local frequency
//
//     f_local(s) = f · profile(t(s)) · (1 + spacingRandom · n(τ(s)))
//
// turns per unit of length, s the arc length along the curl's axis, t the share of the strand,
// `profile` Curl's frequencyProfile (0 to 1, as every ramp), n a smooth noise of the strand's (or the clump's)
// own in [-1, 1], and τ = |f| w its turns so far without the noise, w = ∫0^s profile du the
// "profiled arc" (the axis's arc length weighted by the profile: curl_op.cpp Axis integrates it
// along its 32-sample table, two-point Gauss on each piece between the ramp's keys, as the arc
// length itself). The angle is the integral of it, exactly:
//
//     θ(s) = 2π ∫0^s f_local du + φ = 2π (f w + sign(f) · spacingRandom · S(|f| w)) + φ
//
// S being n's integral, in closed form: n is made of layers each 0 at its cells' ends (cells a
// few turns long), whose integrals over a cell telescope (curlSpacingTurns). So the phase is
// resolution-independent (a function of the axis alone), it never turns back (f_local keeps f's
// sign: |n| < 1), and the noise's scale is in turns, not scene units (scale-invariant). With the
// defaults (a profile of 1 everywhere, spacingRandom 0), w is the arc length and θ = 2π f s + φ.
//
// Curl's weight divides the profile by a stretch along the strand (curl_op.cpp: the turns spread
// where hair hangs below them): w then weighs the arc by profile / stretch, and the rest holds.

#include <cstdint>

#include "groomlab/shared/groom_math.h"

// The functions themselves are shared/groom_math.h's (look.slang includes it for the GPU preview):
// curlHash (a 32-bit integer hash, Wellons' lowbias32), curlSpacingKey (the key, from the bits of
// the curl's four draws: CurlOp::drawFor's, the strand's or the clump's, so it needs no number of
// its own), the layers' lattice, value and integral (curlSpacingLattice, curlSpacingValue,
// curlSpacingIntegral), curlSpacingRate (n), curlSpacingTurns (S), and from them curlAngle,
// curlTurns and curlLocalFrequency.

namespace groomlab {

using shared::curlAngle;
using shared::curlHash;
using shared::curlLocalFrequency;
using shared::curlSpacingKey;
using shared::curlSpacingRate;
using shared::curlSpacingTurns;
using shared::curlTurns;
using shared::kCurlTwoPi;

}  // namespace groomlab
