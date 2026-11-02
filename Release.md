# Releasing Lila

A release is a GitHub release `v<version>` with two files: `Lila-<version>-Setup.exe` and `latest.json`. Lila's updater reads `latest.json` from the newest release, so publishing the release is all it takes for users to be offered the update.

## Release (the usual way)

1. Bump `project(Lila VERSION x.y.z)` in `CMakeLists.txt`, run `python scripts\update_credits_snapshot.py` to refresh the bundled credits, and commit.
2. Push the commit, then tag it and push the tag:
   ```
   git tag v0.2.0
   git push origin v0.2.0
   ```
3. Watch the run under the repo's **Actions** tab (workflow **Release**). When it's green, the release with both files is on the **Releases** page.
4. Steam: build Release locally and upload it with `SteamUpload.cmd` (Steam's login can't run on GitHub).

## Release from this machine (urgent fixes)

Use this when someone is waiting on a fix and the Actions run would take too long, or when the workflow is broken.

1. Bump the version, commit, push. The script refuses uncommitted or unpushed work and never pushes for you.
2. From the repo root:
   ```
   Release.cmd -Notes "Fixes the crash when importing FBX with no skin" -Steam
   ```
   It builds Release, runs `lila_tests`, builds the installer and `latest.json`, publishes the GitHub release, and with `-Steam` uploads to Steam too.
3. To build and check everything without publishing: `Release.cmd -DryRun`. The installer is in `build-installer\`.

First time on a machine: `winget install GitHub.cli` and `gh auth login`. Inno Setup comes from `winget install JRSoftware.InnoSetup`.

## Troubleshooting

**"Tag v0.2.0 doesn't match CMakeLists.txt version"** (Actions) or **"v0.2.0 already exists"** (Release.cmd). The version wasn't bumped, or the tag was reused. Bump the version, commit, tag again.

**"HEAD isn't on origin yet."** Push the commit first.

**"Release build failed. If Lila is running from build\bin\Release, close it."** A running Lila locks its DLLs.

**"gh release create failed."** Run `gh auth login`.

**The Actions run fails in Configure or Build.** The first run builds every vcpkg dependency (USD takes the longest, over an hour); later runs reuse the cache. A Qt download failure means `install-qt-action` doesn't have that Qt version yet: change `version` in `.github/workflows/release.yml`.

**A release is broken.** Publish a fixed version with a higher number. Deleting the release on GitHub makes the previous one the newest again, so updaters stop offering the broken one, but anyone who already installed it keeps it.

**Users aren't offered the update.** They must be on an installer build (Steam and source builds don't check), and the release must not be a draft or pre-release. Help > Check for Updates in Lila shows what it found.

## Options

| `Release.cmd` option | What it does |
| --- | --- |
| `-Notes "..."` | Release notes, shown in Lila's update offer. Without it GitHub generates notes from the commits. |
| `-DryRun` | Build, test and make the installer; publish nothing. Works on uncommitted work. |
| `-SkipBuild` | Use the existing Release build. |
| `-SkipTests` | Don't run `lila_tests`. |
| `-Steam` | Also upload the build to Steam. |

The Actions workflow can also be started by hand for an existing tag: **Actions > Release > Run workflow**. It skips a tag that's already released, so it never clashes with `Release.cmd`.
