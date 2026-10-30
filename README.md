# Lila

3D app for game assets: Maya-style workflow, modern multithreaded core, Unreal-first pipeline.

## Get it running (Windows)

Needs Visual Studio 2022 (C++ workload, CMake 3.28+, vcpkg), Qt 6.11.1 (`msvc2022_64`, plus the Ninja tool from the Qt installer) and Python 3.14. Defaults live in `scripts/env.cmd`; set `LILA_VS`, `LILA_QT_DIR`, `LILA_PYTHON_DIR` or `VCPKG_ROOT` before calling it to override. sccache and lld-link are used when found, and are optional.

```
build.bat                              # configures on first run (vcpkg deps; USD makes this long), then builds
build\bin\RelWithDebInfo\lila.exe      # or: lila.exe <folder>  /  lila.exe scene.lila
```

Don't create the `build` folder by hand; the first `build.bat` makes it. `build.bat <dir> <config>` builds into another folder or configuration.

To build an installer instead, also install Inno Setup 6 (`winget install JRSoftware.InnoSetup`), then:

```
scripts\build.cmd release
scripts\installer\build.cmd            # writes build-installer\Lila-<version>-Setup.exe
```

Tests: `scripts\t.cmd build RelWithDebInfo -LE slow`. `-L slow` runs the slow suites (every shipped material evaluated and linted, about 20 minutes).

### Unreal

1. Copy `ue/LilaLink` into `<Project>/Plugins/LilaLink` and build the project.
2. Open the editor; its log shows `LogLilaLink: Listening on 127.0.0.1:8741`.
3. In Lila: select meshes, File > Send to Unreal, enter content paths. From then on every save re-exports and reimports the mapped objects.

## Troubleshooting

- **vcpkg fails with `Operation REMOVE_DUPLICATES not recognized`:** the vcpkg that ships with Visual Studio is older than Lila's pinned ports. Update Visual Studio 2022 in the Visual Studio Installer (or point `VCPKG_ROOT` at a fresh clone of github.com/microsoft/vcpkg after running its `bootstrap-vcpkg.bat`), delete the `build` folder and run `build.bat` again.
- **Link error LNK1168 while building:** Lila is running. Close it (or `taskkill /IM lila.exe /F`).
- **Viewport stays black:** check the Script Output for a `vulkan:` line. Lila needs a Vulkan 1.3 GPU and driver.
- **Python errors at startup:** `LILA_PYTHON_DIR` must point at the exact Python 3.14 the build used.
- **Script editor shows no completions:** the first open installs basedpyright and ruff into `%LOCALAPPDATA%\Lila\tools`; the status line says when it's ready. It needs pip and a network connection once.
- **Unreal doesn't connect:** run `unrealStatus(scan=True)` in the script editor; check Editor Preferences > Plugins > Lila Link is enabled; the plugin tries ports 8741 to 8748.
- **Save fails on a read-only file:** the file is under Perforce and not checked out; Lila asks to check it out (pref `sourceControl` `/autoCheckout`: `always`, `prompt`, `never`).
- **Crashes:** every crash, including heap corruption and other fail-fast crashes, writes `%APPDATA%\Lila\crashes\<time>_<pid>\report.txt` (what crashed, every thread's stack, the last log lines and commands, system and GPU) plus `minidump.dmp`. Help > Copy Last Crash Report puts the report on the clipboard. No report after a fail-fast crash means `lilawer.dll` or `lilacrash.exe` is missing next to `lila.exe`, or Windows Error Reporting is disabled.

## Tuning

Prefs are JSON in `%APPDATA%\Lila\prefs` (Help > Open Prefs Folder), written as you change things and reloaded when edited outside Lila. A workspace's `.lila/` folder overrides them per project.

- `general.json`: `/scene/upAxis` (`"y"` or `"z"`), `/scene/defaultFormat` (`"binary"` or `"ascii"`), `/autosave/intervalSeconds` (default 30), `/viewport/env/*` (HDRI path, rotation, blur, background), `/manipulator/*` (size, spaces, axis tints; `/manipulator/snap/{move,rotate,scale}/{enabled,step}` and `/manipulator/snap/move/absolute` for the viewport toolbar's increment snapping), `/softSelect/*`, `/references/autoReload`, `/terminal/shell`, `/crash/keepReports` (crash folders kept, default 20), `/crash/fullDump` (full-memory minidumps, default off).
- `hotkeys.json`: every binding, by context (Windows > Hotkey Editor edits it).
- `markingMenus/*.json`, `shelves/*.json`: menus and shelves (Windows > Marking Menu Editor; right-click a shelf).
- `naming.json` (per workspace): naming conventions; joints snake_case, everything else PascalCase by default.
- `sourceControl.json`: Perforce/git settings.
- `unreal.json` (per workspace): Unreal projects and asset mappings.
- MCP for Claude: toggle with the MCP button in the status bar; connection details in `%APPDATA%\Lila\mcp.json`.
