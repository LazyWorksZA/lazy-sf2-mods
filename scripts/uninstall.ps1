<#
.SYNOPSIS
  Removes every file listed in the game folder's lazysf2.manifest.txt, returning the game to stock.
#>
param([string]$GameDir)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$GameDir = Resolve-GameDir $GameDir
$manifest = Join-Path $GameDir $ManifestName
if (-not (Test-Path $manifest)) { Write-Host "Nothing to uninstall: no $ManifestName in $GameDir"; return }

foreach ($name in Get-Content $manifest) {
    $name = $name.Trim()
    if (-not $name -or $name -match '[\\/]|\.\.') { continue }   # only plain file names in the game folder
    $p = Join-Path $GameDir $name
    if (Test-Path $p) { Remove-Item $p -Force; Write-Host "Removed $p" }
}
