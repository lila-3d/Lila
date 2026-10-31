@echo off
rem Configures if needed, then builds every target. Usage: build [build dir] [config], defaults build RelWithDebInfo.
setlocal
set "DIR=%~1"
if "%DIR%"=="" set "DIR=build"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"
call "%~dp0scripts\env.cmd" || exit /b 1
if not exist "%~dp0%DIR%\build.ninja" (
    cmake --preset windows -S "%~dp0." -B "%~dp0%DIR%" || exit /b 1
)
cmake --build "%~dp0%DIR%" --config %CONFIG%
set "RC=%ERRORLEVEL%"
set "T=%TIME: =0%"
if "%RC%"=="0" (echo [done] Build succeeded at %T:~0,5%) else (echo [failed] Build FAILED at %T:~0,5% ^(exit %RC%^))
exit /b %RC%
