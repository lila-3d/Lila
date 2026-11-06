// SPDX-License-Identifier: Apache-2.0
#pragma once

// The one rule every host turns a frame into seconds by (and back): Maya, the CLI, the Arnold
// procedural, the app, Python and the C API all call these, so frame 7 at 23.976 fps is the same
// double everywhere and nodes that read the time (Wind, ImportCurves, Simulate's substeps) give
// the same strands to the bit. A frame rate within a hair of an NTSC one (n * 1000 / 1001: 23.976,
// 29.97, 59.94...) is that rate exactly, however the host spells it (Maya's 23.976023976...,
// another's 23.976): its seconds are frame * 1001 / (n * 1000). Any other rate's are frame / fps.
// A rate of 0 or less (or NaN) is 24.

namespace groomlab {

// `fps` as the rule reads it: an NTSC rate made exact (24000 / 1001.0...), 24 for a bad one.
double canonicalFramesPerSecond(double fps);
// Frame `frame` (fractional for subframes and motion keys) at `fps`, in seconds.
double frameToSeconds(double frame, double fps);
// The frame at `seconds` (fractional): frameToSeconds' inverse.
double secondsToFrame(double seconds, double fps);

}  // namespace groomlab
