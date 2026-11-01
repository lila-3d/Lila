@echo off
rem b [build dir] [config] [target ...]   Quiet build: errors and a summary only, full log in <build dir>\last_build.log.
setlocal
set "HERE=%~dp0"
cd /d "%HERE%.."
if "%~1"=="" (call "%HERE%env.cmd" build >nul 2>&1) else (call "%HERE%env.cmd" "%~1" >nul 2>&1)
if "%~1"=="" (python "%HERE%quiet.py" build build) else (python "%HERE%quiet.py" build %*)
exit /b %ERRORLEVEL%
