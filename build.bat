@echo off
rem Builds every target. Usage: build [build dir] [config], defaults build RelWithDebInfo.
setlocal
set "DIR=%~1"
if "%DIR%"=="" set "DIR=build"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"
call "%~dp0scripts\env.cmd" || exit /b 1
cmake --build "%~dp0%DIR%" --config %CONFIG%
exit /b %ERRORLEVEL%
