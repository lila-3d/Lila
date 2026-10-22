@echo off
rem build.cmd [debug|dev|release]   (default dev)
setlocal
call "%~dp0env.cmd"
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=dev"
cd /d "%~dp0.."
if not exist build\build.ninja cmake --preset windows || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
