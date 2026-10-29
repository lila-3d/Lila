@echo off
rem build.cmd [Release|RelWithDebInfo]   builds build-installer\Lila-<version>-Setup.exe
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Config %CONFIG%
