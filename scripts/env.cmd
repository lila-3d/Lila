@echo off
rem Dev shell: MSVC, Qt, Python, vcpkg. Override any of these before calling.
if not defined LILA_VS set "LILA_VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not defined LILA_QT_DIR set "LILA_QT_DIR=C:\Qt\6.11.1\msvc2022_64"
if not defined LILA_PYTHON_DIR set "LILA_PYTHON_DIR=C:\Python314"
if not defined VCPKG_ROOT set "VCPKG_ROOT=%LILA_VS%\VC\vcpkg"
if not defined SCCACHE_DIR set "SCCACHE_DIR=X:\sccache"
if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=120G"
if defined LILA_MSVC_VER (
    call "%LILA_VS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=%LILA_MSVC_VER% >nul
) else (
    call "%LILA_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
)
set "PATH=C:\Qt\Tools\Ninja;%LILA_QT_DIR%\bin;%LILA_PYTHON_DIR%;%PATH%"
