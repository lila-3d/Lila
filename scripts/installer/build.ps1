# Builds build-installer\Lila-<version>-Setup.exe from an already built config.
param(
    [string]$Config = "Release",
    [string]$Iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$stage = Join-Path $repo "build-installer\content"
$output = Join-Path $repo "build-installer"
if (-not (Test-Path $Iscc)) {
    $Iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    if (-not (Test-Path $Iscc)) { throw "Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup" }
}

$version = (Select-String -Path "$repo\CMakeLists.txt" -Pattern 'project\(Lila VERSION ([0-9.]+)').Matches[0].Groups[1].Value
& "$repo\scripts\steam\stage.ps1" -Config $Config -Destination $stage -AppLocalCrt

& $Iscc /Q "/DAppVersion=$version" "/DSourceDir=$stage" "/DRepoDir=$repo" "/DOutputDir=$output" "$PSScriptRoot\Lila.iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
"Built $output\Lila-$version-Setup.exe"
