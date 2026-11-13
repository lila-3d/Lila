# Overnight decisions (autoremesh branch, 2026-11-13)

Scope from Jared: UV reuse items 1-4 from the retopo rebase, plus the remaining Quad Remesh gaps, unattended, on `autoremesh`, decisions mine.

## 1. Flow-oriented UV islands

- Field: Lila's own `retopo::solveCrossField` on the chart's triangles (curvature-aligned, borders and creases as features). It is Surface-based and already exported; the AutoRemesher field would need a full Parameterizer per island for the same direction.
- Turn: per triangle the field direction mapped into UV through the triangle's affine map, folded four ways, area-weighted, like `gridAngle`.
- Flat islands have no flow of their own (the field there is arbitrary and cost 0.3-0.6% pack density in the 200/2000-island benchmark), so an uncreased island needs mean |k1 - k2| times its size of at least 0.25 before its flow counts.
- Order for a fresh or packed island: edge grid (agreement 0.4, unchanged) beats flow; flow needs agreement 0.5 and may cost at most 15% more box area than the smallest box, else the smallest box. Long side along U either way. Quad meshes therefore keep the grid rule; flow acts on triangle meshes and sculpts.
- Measured: pack benchmark densities identical to before; uvAuto on triangulated Mochi 71.86% vs 71.75%, head and quad meshes identical. The gain is texel direction (limbs and panels along U/V), not density.
- `uvOrient mode=flow` and `mode=auto` (the upright rule) added; the Orient UVs action keeps bbox as its default.

## 2. Flow singularities as cones and darts

- `flowCones(chart)`: the singular triangles of the chart's flow (same solver as item 1, no flatness gate), each at its most curved corner off the border, ranked by the angle defect summed over two rings (per-vertex defect reads 0.005 rad on a fine mesh and was useless as a threshold).
- `uvBff coneSource=flow`: cones from the flow instead of the greedy stretch picker; `cones` caps the count, 0 takes every one bending at least 0.05 rad, kept three edges apart. On a pointed cap it lands on the tip and halves area stretch (1.21 -> 0.51), same as the greedy pick there; it needs no repeated scale solves.
- Auto seams darts (`AutoSeamOptions::darts`, flag `darts` on uvAutoSeams/uvAuto): an island over the distortion limit first tries cuts from its +1 flow points (caps) to its border, cheapest by seam cost, up to 4, clustered within 0.15 sqrt(area), total under 35% of the border; kept only when the darted island flattens under the limit, else it splits as before. An island darts once.
- Measured on Mochi and the head (quad and triangulated, 8 runs): seams 10-22% shorter and 2-6 fewer islands where darts apply, but coverage 4-8% lower (notched islands pack looser) and about 3x the time (2-7 s). So darts are off by default and offered as the flag; the default output is unchanged.
- Found while testing: BFF cones only cut along polygon faces, so a chart with no faces gets cones that do nothing. Real charts have faces; only the test helper needed them.

## 4. Cage paint: own-fold clearance

- Nothing from the remesher fits the per-point solve, so this item is the gap found while looking: clearance ran only between lows. Each low now also traces its own surface: front faces only (the faces a ray leaves from look away), from a few epsilons out.
- Share: another low keeps 0.9 of the gap as before; across a fold of the same low both sides move into one gap, so each keeps 0.45.
- Measured: new fold test 20.6 cm -> 2.25 cm under the slab, open areas unchanged; benchmark miss rates identical (2.856% / 2.876%), time within noise. Open sheets seen from behind don't clamp (back faces).
