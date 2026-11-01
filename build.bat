@echo off
rem Configures if needed, then builds every target. Usage: build [build dir] [config], defaults build RelWithDebInfo.
setlocal
set "DIR=%~1"
if "%DIR%"=="" set "DIR=build"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"
call "%~dp0scripts\env.cmd" "%~dp0%DIR%" || exit /b 1
if not exist "%~dp0%DIR%\build.ninja" (
    cmake --preset windows -S "%~dp0." -B "%~dp0%DIR%" || exit /b 1
)
cmake --build "%~dp0%DIR%" --config %CONFIG%
set "RC=%ERRORLEVEL%"
set "T=%TIME: =0%"
if "%RC%"=="0" (echo [done] Build succeeded at %T:~0,5%) else (echo [failed] Build FAILED at %T:~0,5% ^(exit %RC%^))
if "%RC%"=="0" exit /b 0
set "HINT=If the error mentions a locked or unopenable file, make sure Lila is closed."
tasklist /fi "imagename eq lila.exe" 2>nul | findstr /i "lila.exe" >nul && set "HINT=Lila is running and locks its DLLs. Close it and build again."
rem Our own ninja has exited by now, so any ninja left is another build.
tasklist /fi "imagename eq ninja.exe" 2>nul | findstr /i "ninja.exe" >nul && set "HINT=Another build is running. Two builds in one folder fail with permission denied; wait for it, or pass another build dir."
echo [hint] %HINT%
exit /b %RC%
