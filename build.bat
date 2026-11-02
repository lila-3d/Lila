@echo off
rem Configures if needed, then builds every target. Usage: build [build dir] [config], defaults build RelWithDebInfo.
setlocal
set "DIR=%~1"
if "%DIR%"=="" set "DIR=build"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"
call "%~dp0scripts\env.cmd" || exit /b 1
rem A build dir configured with another toolset reconfigures from scratch; its objects would not link with this one.
if exist "%~dp0%DIR%\CMakeCache.txt" findstr /c:"MSVC/%LILA_MSVC_VER%." "%~dp0%DIR%\CMakeCache.txt" >nul || (
    echo [note] %DIR% was configured with another MSVC toolset; reconfiguring it for %LILA_MSVC_VER%.
    del "%~dp0%DIR%\CMakeCache.txt" "%~dp0%DIR%\build.ninja"
    rmdir /s /q "%~dp0%DIR%\CMakeFiles"
)
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
