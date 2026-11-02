@echo off
rem Prints the MSVC toolset, library paths and linker the build uses, for diagnosing link errors.
setlocal
call "%~dp0env.cmd" || exit /b 1
echo VCToolsVersion=%VCToolsVersion%
echo LIB=%LIB%
where lld-link
where cl
