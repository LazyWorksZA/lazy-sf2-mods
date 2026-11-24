<#
.SYNOPSIS
  Builds the LazySF2 installer: installer\Releases\LazySF2-win.msi (for players) plus Velopack's
  Setup.exe and packages.

.EXAMPLE
  .\installer\build-installer.ps1 -Version 0.1.0 -Clean

.NOTES
  Needs the .NET 10 SDK and vpk 1.2.161 (dotnet tool install -g vpk --version 1.2.161; keep it in
  step with the Velopack package version in LazySF2.Manager.csproj).

  The MSI is a normal installer wizard (welcome, license with third-party notices, finish), installs
  per machine and runs elevated, so its hooks can always write the game folder. The helper it
  installs (LazySF2.exe) is headless and framework-dependent; the installer adds the .NET 10 runtime
  if it is missing. No shortcuts are created: settings are in the game's Options.
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
$meta = Join-Path $PSScriptRoot 'publish-meta'

# 1. The mod itself (x86 winmm.dll proxy).
& (Join-Path $root 'build.ps1')

# 2. The helper, with the mod DLL as its payload.
if (Test-Path $publish) { Remove-Item $publish -Recurse -Force }
dotnet publish (Join-Path $PSScriptRoot 'LazySF2.Manager') -c Release -o $publish -p:Version=$Version
if ($LASTEXITCODE) { throw "dotnet publish failed ($LASTEXITCODE)" }
Remove-Item (Join-Path $publish '*.pdb') -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $publish 'payload') | Out-Null
Copy-Item (Join-Path $root 'build\Release\winmm.dll') (Join-Path $publish 'payload\winmm.dll')
# Our license and the third-party notices (MinHook in winmm.dll, Velopack) travel with the install.
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $publish 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $publish 'THIRD_PARTY_NOTICES.md')

# 3. Installer texts. The license page shows our license followed by the third-party notices.
New-Item -ItemType Directory -Force $meta | Out-Null
$license = Join-Path $meta 'license.md'
$licenseText = "# LazySF2`n`n" + (Get-Content (Join-Path $root 'LICENSE') -Raw).Trim() + "`n`n---`n`n" +
               (Get-Content (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Raw)
[IO.File]::WriteAllText($license, $licenseText, (New-Object Text.UTF8Encoding $false))

# 4. Velopack: MSI wizard + Setup.exe.
if ($Clean -and (Test-Path $releases)) { Remove-Item $releases -Recurse -Force }
if (-not (Get-Command vpk -ErrorAction SilentlyContinue)) { $env:Path += ";$env:USERPROFILE\.dotnet\tools" }
vpk pack --packId LazySF2 --packVersion $Version --packDir $publish --mainExe LazySF2.exe `
    --packTitle 'LazySF2' --packAuthors 'Lazy Works' --runtime win-x64 --framework net10.0-x64-runtime --shortcuts '' `
    --msi --instLocation PerMachine `
    --instWelcome (Join-Path $PSScriptRoot 'welcome.txt') --instLicense $license `
    --instConclusion (Join-Path $PSScriptRoot 'conclusion.txt') `
    --outputDir $releases
if ($LASTEXITCODE) { throw "vpk pack failed ($LASTEXITCODE)" }
Get-ChildItem $releases | Select-Object Name, Length
