<#
.SYNOPSIS
  Builds the Velopack installer: mod DLL + LazySF2 manager app -> installer\Releases\LazySF2-win-Setup.exe

.EXAMPLE
  .\installer\build-installer.ps1 -Version 0.1.0

.NOTES
  Needs the .NET 10 SDK and vpk 1.2.161 (dotnet tool install -g vpk --version 1.2.161; keep it in
  step with the Velopack package version in LazySF2.Manager.csproj). The manager is framework-
  dependent; Setup installs the .NET 10 Desktop Runtime if it is missing.
#>
param(
    [Parameter(Mandatory)] [string]$Version,
    # Delete installer\Releases first. vpk refuses a version that isn't newer than one already there
    # (it keeps old releases to build update packages), so use this only for unpublished local builds.
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$publish = Join-Path $PSScriptRoot 'publish'
$releases = Join-Path $PSScriptRoot 'Releases'

# 1. The mod itself (x86 winmm.dll proxy).
& (Join-Path $root 'build.ps1')

# 2. The manager app, with the mod DLL as its payload.
if (Test-Path $publish) { Remove-Item $publish -Recurse -Force }
dotnet publish (Join-Path $PSScriptRoot 'LazySF2.Manager') -c Release -o $publish -p:Version=$Version
if ($LASTEXITCODE) { throw "dotnet publish failed ($LASTEXITCODE)" }
New-Item -ItemType Directory -Force (Join-Path $publish 'payload') | Out-Null
Copy-Item (Join-Path $root 'build\Release\winmm.dll') (Join-Path $publish 'payload\winmm.dll')
# Our license and the third-party notices (MinHook in winmm.dll, Velopack) travel with the install.
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $publish 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $publish 'THIRD_PARTY_NOTICES.md')

# 3. Velopack package + Setup.exe.
if ($Clean -and (Test-Path $releases)) { Remove-Item $releases -Recurse -Force }
$vpk = Get-Command vpk -ErrorAction SilentlyContinue
if (-not $vpk) { $env:Path += ";$env:USERPROFILE\.dotnet\tools" }
vpk pack --packId LazySF2 --packVersion $Version --packDir $publish --mainExe LazySF2.exe `
    --packTitle 'LazySF2' --packAuthors 'Lazy Works' --framework net10.0-x64-desktop --outputDir $releases
if ($LASTEXITCODE) { throw "vpk pack failed ($LASTEXITCODE)" }
Get-ChildItem $releases | Select-Object Name, Length
