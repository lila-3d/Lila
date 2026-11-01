@echo off
rem t [build dir] [config] [ctest args ...]   Quiet tests: failures and a summary only, full log in <build dir>\last_test.log.
setlocal
set "HERE=%~dp0"
cd /d "%HERE%.."
if "%~1"=="" (call "%HERE%env.cmd" build >nul 2>&1) else (call "%HERE%env.cmd" "%~1" >nul 2>&1)
if "%~1"=="" (python "%HERE%quiet.py" test build) else (python "%HERE%quiet.py" test %*)
exit /b %ERRORLEVEL%
