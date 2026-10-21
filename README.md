# Lila

Game-asset DCC: Maya-style workflow, modern multithreaded core, Unreal-first pipeline.

## Get it running (Windows)

Needs Visual Studio 2022, Qt 6.11.1 (`msvc2022_64`) and Python 3.14. Defaults live in `scripts/env.cmd`; set `LILA_QT_DIR`, `LILA_PYTHON_DIR` or `VCPKG_ROOT` before calling it to override.

```
scripts\build.cmd                      # first run fetches vcpkg deps (a few minutes)
build\bin\RelWithDebInfo\lila.exe      # or: lila.exe <folder>  /  lila.exe scene.lila
```

Then: Shift+right-click in the viewport for primitives, W/E/R to move/rotate/scale, F8 to F11 for object/vertex/edge/face, Shift+right-click on a mesh for modeling ops, tap Space for four views, hold Space for the hotbox, hold F1 for the hotkey cheat sheet.

Tests: `ctest --test-dir build -C RelWithDebInfo` (from a shell set up by `scripts\env.cmd`).

### Unreal

1. Copy `ue/LilaLink` into `<Project>/Plugins/LilaLink` and build the project.
2. Open the editor; its log shows `LogLilaLink: Listening on 127.0.0.1:8741`.
3. In Lila: select meshes, File > Send to Unreal, enter content paths. From then on every save re-exports and reimports the mapped objects.

## Troubleshooting

- **Link error LNK1168 while building:** Lila is running. Close it (or `taskkill /IM lila.exe /F`).
- **Viewport stays black:** check the Script Output for a `vulkan:` line. Lila needs a Vulkan 1.3 GPU and driver.
- **Python errors at startup:** `LILA_PYTHON_DIR` must point at the exact Python 3.14 the build used.
- **Script editor shows no completions:** the first open installs basedpyright and ruff into `%LOCALAPPDATA%\Lila\tools`; the status line says when it's ready. It needs pip and a network connection once.
- **Unreal doesn't connect:** run `unrealStatus(scan=True)` in the script editor; check Editor Preferences > Plugins > Lila Link is enabled; the plugin tries ports 8741 to 8748.
- **Save fails on a read-only file:** the file is under Perforce and not checked out; Lila asks to check it out (pref `sourceControl` `/autoCheckout`: `always`, `prompt`, `never`).
- **Crashes:** a stack trace goes to `%APPDATA%\Lila\crash.log`.

## Tuning

Prefs are JSON in `%APPDATA%\Lila\prefs` (Help > Open Prefs Folder), written as you change things and reloaded when edited outside Lila. A workspace's `.lila/` folder overrides them per project.

- `general.json`: `/scene/upAxis` (`"y"` or `"z"`), `/scene/defaultFormat` (`"binary"` or `"ascii"`), `/autosave/intervalMinutes`, `/viewport/env/*` (HDRI path, rotation, blur, background), `/manipulator/*` (size, spaces, axis tints), `/softSelect/*`, `/references/autoReload`, `/terminal/shell`.
- `hotkeys.json`: every binding, by context (Windows > Hotkey Editor edits it).
- `markingMenus/*.json`, `shelves/*.json`: menus and shelves (Windows > Marking Menu Editor; right-click a shelf).
- `naming.json` (per workspace): naming conventions; joints snake_case, everything else PascalCase by default.
- `sourceControl.json`: Perforce/git settings.
- `unreal.json` (per workspace): Unreal projects and asset mappings.
- MCP for Claude: toggle with the MCP button in the status bar; connection details in `%APPDATA%\Lila\mcp.json`.
