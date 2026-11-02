@echo off
rem Dev shell: MSVC, Python, vcpkg (which builds Qt too). Override any of these before calling.
if not defined LILA_VS set "LILA_VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
for %%P in ("C:\Python314" "%LOCALAPPDATA%\Python\pythoncore-3.14-64" "%LOCALAPPDATA%\Programs\Python\Python314" "%ProgramFiles%\Python314") do (
    if not defined LILA_PYTHON_DIR if exist "%%~P\libs\python314.lib" set "LILA_PYTHON_DIR=%%~P"
)
if not defined LILA_PYTHON_DIR call "%~dp0fetch-python.cmd" || exit /b 1
if not defined VCPKG_ROOT set "VCPKG_ROOT=%LILA_VS%\VC\vcpkg"
if not defined SCCACHE_DIR set "SCCACHE_DIR=X:\sccache"
if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=120G"
rem The toolset pinned in the vcpkg triplet, so Lila and its dependencies always match.
for /f "tokens=3 delims=() " %%V in ('findstr /c:"VCPKG_PLATFORM_TOOLSET_VERSION" "%~dp0..\cmake\triplets\x64-windows.cmake"') do set "LILA_MSVC_VER=%%V"
set "LILA_VCVARS_LOG=%TEMP%\lila_vcvars_%RANDOM%%RANDOM%.log"
call "%LILA_VS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=%LILA_MSVC_VER% >"%LILA_VCVARS_LOG%" 2>&1
where cl >nul 2>&1 || (
    findstr /i "error" "%LILA_VCVARS_LOG%"
    echo [error] MSVC %LILA_MSVC_VER% is missing under "%LILA_VS%". Add it in the Visual Studio Installer ^(Individual components, MSVC v143 x64/x86 build tools, latest^), or set LILA_VS.
    del "%LILA_VCVARS_LOG%" >nul 2>&1
    exit /b 1
)
del "%LILA_VCVARS_LOG%" >nul 2>&1
set "PATH=%LILA_PYTHON_DIR%;%PATH%"
rem Without Visual Studio's CMake tools there is no Ninja on PATH; vcpkg downloads its own.
where ninja >nul 2>&1 || if exist "%VCPKG_ROOT%\vcpkg.exe" for /f "delims=" %%N in ('call "%VCPKG_ROOT%\vcpkg.exe" fetch ninja') do if exist "%%~N" set "PATH=%%~dpN;%PATH%"
where ninja >nul 2>&1 || echo [warn] No Ninja found. Install Visual Studio's "C++ CMake tools for Windows" or put ninja.exe on PATH.
