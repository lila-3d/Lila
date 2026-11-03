@echo off
rem b [build dir] [config] [-nounity] [target ...]   Quiet build: errors and a summary only, full log in <build dir>\last_build.log.
setlocal
set "HERE=%~dp0"
call "%HERE%env.cmd" >nul 2>&1
cd /d "%HERE%.."
if "%~1"=="" (python "%HERE%quiet.py" build build) else (python "%HERE%quiet.py" build %*)
exit /b %ERRORLEVEL%
