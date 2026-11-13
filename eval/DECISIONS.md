# Overnight decisions (autoremesh branch, 2026-11-13)

Scope from Jared: UV reuse items 1-4 from the retopo rebase, plus the remaining Quad Remesh gaps, unattended, on `autoremesh`, decisions mine.

## 1. Flow-oriented UV islands

- Field: Lila's own `retopo::solveCrossField` on the chart's triangles (curvature-aligned, borders and creases as features). It is Surface-based and already exported; the AutoRemesher field would need a full Parameterizer per island for the same direction.
- Turn: per triangle the field direction mapped into UV through the triangle's affine map, folded four ways, area-weighted, like `gridAngle`.
- Flat islands have no flow of their own (the field there is arbitrary and cost 0.3-0.6% pack density in the 200/2000-island benchmark), so an uncreased island needs mean |k1 - k2| times its size of at least 0.25 before its flow counts.
- Order for a fresh or packed island: edge grid (agreement 0.4, unchanged) beats flow; flow needs agreement 0.5 and may cost at most 15% more box area than the smallest box, else the smallest box. Long side along U either way. Quad meshes therefore keep the grid rule; flow acts on triangle meshes and sculpts.
- Measured: pack benchmark densities identical to before; uvAuto on triangulated Mochi 71.86% vs 71.75%, head and quad meshes identical. The gain is texel direction (limbs and panels along U/V), not density.
- `uvOrient mode=flow` and `mode=auto` (the upright rule) added; the Orient UVs action keeps bbox as its default.
