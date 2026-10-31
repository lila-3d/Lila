# Product

<!-- impeccable:product-schema 1 -->

## Platform

desktop

Native desktop app (C++20, Qt 6 widgets as UI shell, Vulkan viewport). Windows now; macOS (MoltenVK) and Linux planned. Not one of web/ios/android/adaptive: Lila draws its own themed UI, not OS-native design language. The website and user docs (`X:\Lila-3D`: `index.html`, `docs/`) are web and are governed by this record too.

## Users

Primary: game artists at Unreal Engine studios who work in Maya today and carry its muscle memory (hotkeys, marking menus, shelves, channel box, Attribute Editor). They model, UV, rig, animate and ship assets into UE all day; Lila must feel familiar on day one with no retraining. Studio pilots are not a site goal: Jared is in Auckland and few studios can be visited in person.

Secondary: technical artists and pipeline engineers who script and extend Lila (Python API, C++ SDK, MCP, LilaLink). Further out: indie developers buying the Steam build, and large-world/sim/CAD studios drawn by double-precision vertices.

## Product Purpose

Lila is a 3D app for games with a Maya workflow on a modern, multithreaded core and a first-class Unreal pipeline. It exists because Maya is slow, single-threaded, dated and has a poor project system, and because the tools its author could not build as Maya plugins become possible when he owns the core. Success: studio artists switch without retraining and finish assets faster, and studios adopt it because a buyout or abandonment cannot take it away. Before release, the site's job is traction: following @Vaei00 on X for the release is the primary action.

## Positioning

- Maya muscle memory transfers; modeling/UV tools aim at the Blender/Modo/RizomUV bar, not Maya's.
- Unreal pipeline built in: LilaLink plugin with live sync, export converting per target (UE5 Z-up LH, UE6 Y-up LH).
- Double-precision vertices with bit-exact OBJ/FBX/.lila round trips (Maya and Blender store float32).
- Pipeline tooling: typed OO Python API, MIT SDK, built-in MCP, native Perforce.
- Free to use; source-available (never "open source"); license guarantees survive a buyout (perpetual grant, successors clause, Apache-2.0 continuity clause). Steam build is US$24.99 as convenience plus donation.

## Operating Context

Long sessions in a dense multi-panel desktop layout around a realtime Vulkan viewport. Any opened folder with `.lila/` state is a workspace; no forced project. Studio settings: Perforce checkout/submit, UE editor running alongside, scripts and batch (`lilabatch`). Prefs are JSON, written through live and hot-reloaded.

## Capabilities and Constraints

- Realtime viewport only; no offline renderer.
- Y-up default, Z-up option, right-handed internal, centimetres.
- Symmetry perfect by construction across modeling, UVs, weights, joints.
- Unique scene-wide names; per-workspace naming conventions.
- Add-ons are **extensions** (C++ and Python). "Plugin" only for UE, Qt or Maya plugins.
- Python API: long flags only, no query mode, declarative `lila.ui` on PySide6; never copy Maya's Python UI (formLayout).
- Lila ships zero generative AI (content generators like Meshy, Midjourney, image/text-to-3D) in core or first-party extensions. Machine learning that assists the artist's own work is not generative AI and is allowed: auto-posing from a few pins, real-time learned motion, crowds driven by it. Agent workflows over MCP drive Lila's own tools and are fine.
- Steam Early Access only at full-release quality.

## Brand Commitments

- Name: Lila, Sanskrit for "divine play" or "creation" (shown in About and README).
- Call it a "3D app" (docs: "the 3D app for games and film"). Never "DCC".
- Colour and perception: UI must not bias the artist's colour perception. The default theme keeps panel chrome uncoloured, while editing feedback (component modes, selection, hover, gizmos) is coloured. A monochrome toggle removes that editing colour too. Data displays (vertex colours, weight ramp, soft-select falloff, heatmaps) are always coloured. Users may pick themes with lots of colour; the default stays neutral.
- Viewport: beautiful but neutral, HDRI background, high-quality AA.
- Compare with Maya only where measured or verified; avoid "Maya can't do this" claims. The site names Maya, not Blender.
- Promo videos: Lila icon bottom-left, caption bar, end card, dark theme.

## Evidence on Hand

- Benchmarks vs Maya/Blender on `manyships.obj` (see memory/plan notes).
- Double-precision round-trip test: `lila_fbx_tests` "cad-precision".
- Hero screenshot `X:\Lila-3D\assets\screenshot.webp`; other site media slots are placeholders awaiting real captures.
- No customer testimonials, studio logos, press or adoption numbers yet. Do not fabricate any.

## Product Principles

1. Familiar first: a Maya artist is productive on day one; deviate from Maya only where it is measurably better.
2. Never block the artist: multithread anything with a reason to, keep interaction latency invisible.
3. Neutral by default: the interface serves colour-critical work and stays out of the way.
4. Pipeline is the product: every asset path ends cleanly in Unreal.
5. Owned and safe to adopt: the license and source availability make a studio's investment durable.
