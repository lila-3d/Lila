# Lila

Maya-workflow 3D app for game work with a first-class Unreal pipeline. C++20, Qt 6 UI shell, Vulkan, TBB, embedded Python, built-in MCP.

Plan: `C:\Users\Jared\.claude\plans\enumerated-watching-quilt.md` is an index; each stage is its own file in `plans\lila\`. Read only the stage for the work at hand.

## Where things live

- `src/core` platform, prefs, logging. `src/ops` command registry, undo, option boxes (`CommandOptions`). `src/scene` DG: nodes, plugs (`Plug.h`: `AttrDef`, `AttrFlags`), graph, transforms, rig, anim. `src/geo` half-edge mesh and mesh ops. `src/curve` curves and planar/text nodes. `src/gn` geometry nodes. `src/uv`, `src/bake`, `src/tex`, `src/retopo`, `src/sculpt`, `src/deform`, `src/vcolor`, `src/seq`, `src/fbx`, `src/gltf`, `src/maya`, `src/io` file formats and references. `src/render` Vulkan. `src/ui` Qt. `src/mcp` MCP server. `src/link` + `ue/LilaLink` Unreal link.
- Extensions: `extensions/<Name>` (SDK headers only, `sdk/include/lila`). Built-in registrations use the same registries with owner "lila".
- DLLs: LilaFoundation (core, ops), LilaScene (scene and everything non-UI), LilaUI. Exported symbols need `LILA_<LIB>_API`.

## Adding a feature (the usual touch points)

- Command: `ops::registerCommand` (`src/ops/Command.h`), usually in a `register*Commands()` next to the feature. Typed flags, long names, a doc string. Undoable via the command's do/undo.
- Node type and attributes: `AttrDef` list with `AttrFlags` (`src/scene/Plug.h`); see `registerPlanar` in `src/curve/PlanarNodes.cpp` for a compact example.
- Option box: `ops::options::setOverride` (labels, ranges, layout, editors) in `src/ops/CommandOptions.*`, fields drawn by `src/ui/FlagFields.cpp`.
- Attribute Editor: plain attributes appear automatically; a custom section is `attributeEditor::addSection(type, title, factory)` (`src/ui/AttributeEditor.h`).
- Action, menu item, hotkey: `actions::add` (`src/ui/Actions.h`). Panels: `panels::add` (`src/ui/Panels.h`).
- Tests: doctest suites under `tests/` (`lila_tests` plus per-area executables such as `tests/curve`, `tests/docks`); add new files to that folder's CMakeLists.
- Python stubs `python/lila/ops.pyi` regenerate at startup of lila/lilabatch; don't hand-edit.
- User docs: `X:\Lila-3D\docs` (not this repo). Essentials only for day-one things; document only what the UI doesn't already show.

## Build and test

- Configure and build from `scripts\env.cmd`: `cmake --build build --config RelWithDebInfo`. Build all targets before handing back; partial builds leave extension DLLs stale.
- If Jared's lila.exe is running (LNK1168), never kill it: build in `build-night` instead. Never launch his lila.exe either.
- Run executables only from `build/` or `build-night` (new paths trigger firewall prompts). No popups, ever.
- Test instances: set `LILA_CONFIG_DIR=<scratch dir>` and `LILA_MCP_PORT=<free port>`, drive over MCP, kill only that instance's PID afterwards.
- Known failing, not ours unless touched: `lila_seq_tests` (exportVertexAnimation png), `lila_sdk_build_examples` (blend_scene needs zstd), `lila_material_pack_tests` timeout.
- PCH + unity builds are on: use `Q_EMIT`/`Q_SIGNALS`/`Q_SLOTS`; a unity clash means rename the file-local helper or add the source to `cmake/LilaUnitySkip.cmake`.
- Line endings are mixed per file. `sed -i` in git-bash strips CRLF; restore with `sed -i 's/$/\r/'` or edit with the Edit tool.
- Other Claude sessions often work in this repo at the same time. Check `git status` and peer sessions before large moves or renames of shared files.

## Verifying UI work cheaply

- Check state, not pixels, by default: over MCP `execute_python`, read attributes (`ops.getAttr`), widget state (find widgets by class and read text, checked state, enabled, items) and command results. Assert the numbers.
- Take a screenshot (`captureView`, or a widget grab) only where the visual result is the point: viewport drawing, layout, styling, icons. One capture at the end of a check, not one per step. `captureView` doesn't draw curves.

## Token hygiene

- Several sources are large (`src/ui/ViewportWidget*.cpp`, `src/ui/MainWindow*.cpp`). Grep for the symbol, then Read with offset/limit; don't read whole files.
- Grep build and test logs for `error|FAILED|LNK` instead of printing them.
- Commit only when asked: subject line only.
