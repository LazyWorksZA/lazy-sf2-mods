<#
.SYNOPSIS
  Installs the mod into an SF2 game folder: copies winmm.dll next to the exe and records every
  file it adds in lazysf2.manifest.txt, so uninstall.ps1 can remove exactly those files.

.EXAMPLE
  .\scripts\install.ps1                       # game folder from the registry, Release build
  .\scripts\install.ps1 -GameDir 'D:\SF2' -Config Debug
#>
param(
    [string]$GameDir,
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$GameDir = Resolve-GameDir $GameDir
$src = Join-Path (Split-Path $PSScriptRoot) "build\$Config\winmm.dll"
if (-not (Test-Path $src)) { throw "Build output not found: $src (run .\build.ps1 first)" }

$manifest = Join-Path $GameDir $ManifestName
$dst = Join-Path $GameDir 'winmm.dll'
if ((Test-Path $dst) -and -not (Test-Path $manifest)) {
    throw "$dst already exists and was not installed by this mod (no $ManifestName). Not overwriting it."
}

Copy-Item $src $dst -Force
# Everything the mod may create in the game folder; lazysf2.log is written by the DLL at runtime.
@('winmm.dll', 'lazysf2.log', $ManifestName) | Set-Content -Path $manifest -Encoding utf8
Write-Host "Installed $Config build to $GameDir"
Write-Host "Log after a run: $(Join-Path $GameDir 'lazysf2.log')"
