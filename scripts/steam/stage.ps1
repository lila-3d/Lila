# Stages a self-contained Lila install into build-steam\content for SteamPipe.
param(
    [string]$Config = "Release",
    [string]$PythonDir = $(if ($env:LILA_PYTHON_DIR) { $env:LILA_PYTHON_DIR } else { "C:\Python314" })
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$bin = Join-Path $repo "build\bin\$Config"
$out = Join-Path $repo "build-steam\content"
if (-not (Test-Path "$bin\lila.exe")) { throw "No lila.exe in $bin. Build $Config first." }

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory $out | Out-Null

# Test executables and build leftovers stay behind.
robocopy $bin $out /E /NFL /NDL /NJH /NJS /NP `
    /XF *.pdb *.ilk *.old* *.iconbak imgui.ini *_tests.exe lila_crashtest.exe | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy bin failed" }

robocopy "$repo\resources" "$out\resources" /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy "$repo\python" "$out\python" /E /NFL /NDL /NJH /NJS /NP /XD tests __pycache__ | Out-Null

# Embedded interpreter: stdlib plus numpy, which Lila's own scripts use.
$py = "$out\python-runtime"
robocopy "$PythonDir\Lib" "$py\Lib" /E /NFL /NDL /NJH /NJS /NP /XD site-packages test idlelib turtledemo __pycache__ | Out-Null
robocopy "$PythonDir\DLLs" "$py\DLLs" /E /NFL /NDL /NJH /NJS /NP | Out-Null
foreach ($pkg in "numpy", "numpy.libs") {
    robocopy "$PythonDir\Lib\site-packages\$pkg" "$py\Lib\site-packages\$pkg" /E /NFL /NDL /NJH /NJS /NP /XD __pycache__ tests | Out-Null
}
Copy-Item "$PythonDir\python.exe", "$PythonDir\pythonw.exe", "$PythonDir\python3.dll", "$PythonDir\python314.dll", "$PythonDir\LICENSE.txt" $py
Copy-Item "$repo\LICENSE", "$repo\THIRD-PARTY-NOTICES.md" $out

$size = (Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
"Staged {0:N0} MB to {1}" -f $size, $out
