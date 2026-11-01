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
set "LILA_VCVARS_LOG=%TEMP%\lila_vcvars_%RANDOM%%RANDOM%.log"
if defined LILA_MSVC_VER (
    call "%LILA_VS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=%LILA_MSVC_VER% >"%LILA_VCVARS_LOG%" 2>&1
) else (
    call "%LILA_VS%\VC\Auxiliary\Build\vcvars64.bat" >"%LILA_VCVARS_LOG%" 2>&1
)
where cl >nul 2>&1 || (
    findstr /i "error" "%LILA_VCVARS_LOG%"
    echo [error] vcvars64 under "%LILA_VS%" did not set up MSVC. Repair Visual Studio, or set LILA_VS or LILA_MSVC_VER.
    del "%LILA_VCVARS_LOG%" >nul 2>&1
    exit /b 1
)
del "%LILA_VCVARS_LOG%" >nul 2>&1
set "PATH=%LILA_PYTHON_DIR%;%PATH%"
