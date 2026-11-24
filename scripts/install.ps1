<#
.SYNOPSIS
  Installs the mod into an SF2 game folder: copies winmm.dll next to the exe and records every
  file it adds in lazysf2.manifest.txt, so uninstall.ps1 can remove exactly those files.

.EXAMPLE
  .\scripts\install.ps1                       # game folder from the registry, Release build
  .\scripts\install.ps1 -GameDir 'D:\SF2' -Config Debug
  .\scripts\install.ps1 -MouseMode Cursor     # game pointer matches the Windows cursor (recommended)
#>
param(
    [string]$GameDir,
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    # Writes lazysf2.ini. Omit to keep an existing lazysf2.ini unchanged.
    [ValidateSet('Stock', 'Cursor')] [string]$MouseMode
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
if ($MouseMode) {
    @('[Mouse]', "Mode=$MouseMode") | Set-Content -Path (Join-Path $GameDir 'lazysf2.ini') -Encoding ascii
    Write-Host "lazysf2.ini: [Mouse] Mode=$MouseMode"
}
# Everything the mod may create in the game folder; lazysf2.log is written by the DLL at runtime.
@('winmm.dll', 'lazysf2.ini', 'lazysf2.log', $ManifestName) | Set-Content -Path $manifest -Encoding utf8
Write-Host "Installed $Config build to $GameDir"
Write-Host "Log after a run: $(Join-Path $GameDir 'lazysf2.log')"
