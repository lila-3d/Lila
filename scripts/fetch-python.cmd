@echo off
rem Points LILA_PYTHON_DIR at a repo-local Python, downloading python.org's NuGet build (headers and import libraries included) on first use.
set "LILA_PY_VER=3.14.10"
set "LILA_PYTHON_DIR=%~dp0..\.deps\python-%LILA_PY_VER%"
for %%I in ("%LILA_PYTHON_DIR%") do set "LILA_PYTHON_DIR=%%~fI"
if exist "%LILA_PYTHON_DIR%\python.exe" exit /b 0

echo Downloading Python %LILA_PY_VER% into %LILA_PYTHON_DIR%
set "LILA_PY_TMP=%LILA_PYTHON_DIR%.tmp"
if exist "%LILA_PY_TMP%" rmdir /s /q "%LILA_PY_TMP%"
mkdir "%LILA_PY_TMP%" || exit /b 1
"%SystemRoot%\System32\curl.exe" -sSfL -o "%LILA_PY_TMP%\python.nupkg" "https://api.nuget.org/v3-flatcontainer/python/%LILA_PY_VER%/python.%LILA_PY_VER%.nupkg" || goto fail
"%SystemRoot%\System32\tar.exe" -xf "%LILA_PY_TMP%\python.nupkg" -C "%LILA_PY_TMP%" tools || goto fail
"%LILA_PY_TMP%\tools\python.exe" -m pip install --quiet --disable-pip-version-check --no-warn-script-location numpy || goto fail
move "%LILA_PY_TMP%\tools" "%LILA_PYTHON_DIR%" >nul || goto fail
rmdir /s /q "%LILA_PY_TMP%"
exit /b 0

:fail
echo [failed] Could not set up Python %LILA_PY_VER%. Install Python 3.14 and set LILA_PYTHON_DIR to it instead.
rmdir /s /q "%LILA_PY_TMP%"
exit /b 1
