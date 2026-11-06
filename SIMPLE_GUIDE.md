# Simple guide

Lila has no download yet. This guide builds it on your Windows PC by typing a few commands. The first build takes a few hours and needs about 100 GB of free disk space.

## 1. Install the tools (once)

Press the Windows key, type `cmd`, and open **Command Prompt**. Paste these two lines one at a time and press Enter after each. Accept any prompts that appear.

```bash
winget install Git.Git
winget install Microsoft.VisualStudio.2022.Community --override "--wait --passive --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Vcpkg --includeRecommended"
```

The second one takes a while. When both finish, close Command Prompt.

## 2. Download and build Lila (once)

> [!WARNING]
> Ensure you have no `C:\Lila` directory, if you do, you'll need to delete it first before cloning

Open a new **Command Prompt** and run:

```bash
cd C:\
git clone https://github.com/lila-3d/Lila.git
cd Lila
build.bat
```

Leave it running. It's done when it prints `[done] Build succeeded`.

## 3. Start Lila

```
C:\Lila\build\bin\RelWithDebInfo\lila.exe
```

Tip: right-click that file in File Explorer and choose **Send to > Desktop (create shortcut)**.

## Get the latest version

Lila changes daily. Close Lila, open **Command Prompt**, and run:

```bash
cd C:\Lila
git pull
build.bat
```

Do this before reporting a bug: it may already be fixed.

## Report a bug

Include the version you're on. Get it with:

```bash
cd C:\Lila
git log -1 --format=%h
```

It prints a short code like `235046cd`. Paste it into your report.

## If something goes wrong

- **`'git' is not recognized` or `'winget' is not recognized`:** close Command Prompt and open a new one. If `winget` still isn't found, install **App Installer** from the Microsoft Store.
- **`MSVC ... is missing`:** open **Visual Studio Installer** from the Start menu, click **Update** next to Visual Studio 2022, then run `build.bat` again.
- **`[failed] Build FAILED`:** read the `[hint]` line under it. If Lila is open, close it and run `build.bat` again.
- **Anything else:** copy the last 20 lines of the window into `#issues` on the [Lila Dev Discord](https://discord.gg/kx3jxkksmf). The [README troubleshooting](README.md#troubleshooting) covers more cases.
