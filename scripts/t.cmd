@echo off
rem t [build dir] [config] [ctest args ...]   Quiet tests: failures and a summary only, full log in <build dir>\last_test.log.
setlocal
set "HERE=%~dp0"
call "%HERE%env.cmd" >nul 2>&1
cd /d "%HERE%.."
if "%~1"=="" (python "%HERE%quiet.py" test build) else (python "%HERE%quiet.py" test %*)
exit /b %ERRORLEVEL%
