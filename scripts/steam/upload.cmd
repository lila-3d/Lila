@echo off
rem upload.cmd [Release|RelWithDebInfo]   stages the build and uploads it to Steam app 5520720
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0upload.ps1" -Config %CONFIG%
