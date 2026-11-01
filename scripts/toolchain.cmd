@echo off
rem toolchain [build dir]   Prints the MSVC toolset, library paths and linker that build dir gets, for diagnosing link errors.
setlocal
cd /d "%~dp0.."
if "%~1"=="" (call "%~dp0env.cmd" build) else (call "%~dp0env.cmd" "%~1")
if errorlevel 1 exit /b 1
echo VCToolsVersion=%VCToolsVersion%
echo LIB=%LIB%
where lld-link
where cl
