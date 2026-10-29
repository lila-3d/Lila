@echo off
rem Release.cmd [-Notes "..."] [-DryRun] [-SkipBuild] [-SkipTests] [-Steam]
rem Builds, tests and publishes the installer and latest.json as a GitHub release. See scripts\release\release.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\release\release.ps1" %*
