# Stages Lila and uploads it to Steam app 5520720 with the login saved by SteamPipe GUI.
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$builder = "$repo\third_party\steamworks\sdk\tools\ContentBuilder"
$out = "$repo\build-steam"

# SteamPipe GUI keeps one user.config per exe path; take the newest that points at this ContentBuilder.
$settings = $null
foreach ($file in Get-ChildItem "$env:LOCALAPPDATA\Valve\SteamPipeGUI.exe_Url_*" -Recurse -Filter user.config | Sort-Object LastWriteTime -Descending) {
    $s = @{}
    foreach ($node in ([xml](Get-Content $file.FullName -Raw)).SelectNodes("//setting")) { $s[$node.name] = $node.value.Trim() }
    if ($s.ContentBuilderPath.TrimEnd('\') -eq $builder) { $settings = $s; break }
}
if (-not $settings -or -not $settings.SteamLogin -or -not $settings.SteamPassword) {
    throw "No saved login in SteamPipe GUI for $builder. Open SteamPipe.lnk, fill it in with Save Password on."
}

& "$PSScriptRoot\stage.ps1" -Config $Config

$rev = (git -C $repo rev-parse --short HEAD).Trim()
$vdf = "$out\app_build.vdf"
@"
"AppBuild"
{
	"AppID" "5520720"
	"Desc" "Lila $rev $Config"
	"ContentRoot" "$out\content\"
	"BuildOutput" "$out\output\"
	"SetLive" "$($settings.Branch)"
	"Preview" "0"
	"Depots"
	{
		"5520721"
		{
			"FileMapping"
			{
				"LocalPath" "*"
				"DepotPath" "."
				"Recursive" "1"
			}
			"FileExclusion" "*.pdb"
		}
	}
}
"@ | Set-Content $vdf -Encoding ASCII

& "$builder\builder\steamcmd.exe" +login $settings.SteamLogin $settings.SteamPassword +run_app_build $vdf +quit
if ($LASTEXITCODE -ne 0) { throw "steamcmd failed ($LASTEXITCODE). Logs in $out\output" }
"Uploaded Lila $rev $Config"
