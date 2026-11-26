<#
.SYNOPSIS
  Builds the Lazy SF2 Patch installer: installer\Releases\LazySF2Patch-win.msi (for players) plus Velopack's
  Setup.exe and packages.

.EXAMPLE
  .\installer\build-installer.ps1 -Version 0.1.0 -Clean

.NOTES
  Needs the .NET 10 SDK and vpk 1.2.161 (dotnet tool install -g vpk --version 1.2.161; keep it in
  step with the Velopack package version in LazyWorks.SF2Patch.Manager.csproj).

  The MSI is a normal installer wizard (welcome, license with third-party notices, finish), installs
  per machine and runs elevated, so its hooks can always write the game folder. The helper it
  installs (LazySF2Patch.exe) is headless and framework-dependent; the installer adds the .NET 10 runtime
  if it is missing. One Start-menu shortcut (Lazy SF2 Patch) opens the helper's folder chooser for installs the detection can't find; settings are in the game's Options.
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
dotnet publish (Join-Path $PSScriptRoot 'LazyWorks.SF2Patch.Manager') -c Release -o $publish -p:Version=$Version
if ($LASTEXITCODE) { throw "dotnet publish failed ($LASTEXITCODE)" }
Remove-Item (Join-Path $publish '*.pdb') -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $publish 'payload') | Out-Null
Copy-Item (Join-Path $root 'build\Release\winmm.dll') (Join-Path $publish 'payload\winmm.dll')
# Our license and the third-party notices (MinHook and ENet in winmm.dll, Velopack) travel with the install.
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $publish 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $publish 'THIRD_PARTY_NOTICES.md')

# 3. Installer texts. The license page shows our license followed by the third-party notices.
New-Item -ItemType Directory -Force $meta | Out-Null
$license = Join-Path $meta 'license.md'
$licenseText = "# Lazy SF2 Patch`n`n" + (Get-Content (Join-Path $root 'LICENSE') -Raw).Trim() + "`n`n---`n`n" +
               (Get-Content (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Raw)
[IO.File]::WriteAllText($license, $licenseText, (New-Object Text.UTF8Encoding $false))

# 4. Velopack: MSI wizard + Setup.exe.
if ($Clean -and (Test-Path $releases)) { Remove-Item $releases -Recurse -Force }
if (-not (Get-Command vpk -ErrorAction SilentlyContinue)) { $env:Path += ";$env:USERPROFILE\.dotnet\tools" }
vpk pack --packId LazySF2Patch --packVersion $Version --packDir $publish --mainExe LazySF2Patch.exe `
    --packTitle 'Lazy SF2 Patch' --packAuthors 'Lazy Works' --icon (Join-Path $PSScriptRoot 'assets\app.ico') --runtime win-x64 --framework net10.0-x64-runtime --shortcuts StartMenuRoot `
    --msi --instLocation PerMachine `
    --instWelcome (Join-Path $PSScriptRoot 'welcome.txt') --instLicense $license `
    --instConclusion (Join-Path $PSScriptRoot 'conclusion.txt') `
    --outputDir $releases
if ($LASTEXITCODE) { throw "vpk pack failed ($LASTEXITCODE)" }

# 5. Velopack's MSI sets no icon for Installed apps: add ours (Icon table + ARPPRODUCTICON).
function Set-MsiArpIcon([string]$msi, [string]$ico) {
    $wi = New-Object -ComObject WindowsInstaller.Installer
    $db = $wi.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $wi, @($msi, 1))   # 1 = transact
    function Invoke-Sql([string]$sql, $record = $null) {
        $view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @($sql))
        $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $(if ($record) { @($record) } else { $null }))
        $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null)
    }
    $tables = $db.GetType().InvokeMember('TablePersistent', 'GetProperty', $null, $db, @('Icon'))
    if ($tables -ne 1) { Invoke-Sql 'CREATE TABLE `Icon` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL PRIMARY KEY `Name`)' }
    $rec = $wi.GetType().InvokeMember('CreateRecord', 'InvokeMethod', $null, $wi, @(2))
    $rec.GetType().InvokeMember('StringData', 'SetProperty', $null, $rec, @(1, 'app.ico'))
    $rec.GetType().InvokeMember('SetStream', 'InvokeMethod', $null, $rec, @(2, $ico))
    $view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @('INSERT INTO `Icon` (`Name`, `Data`) VALUES (?, ?)'))
    $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, @($rec))
    $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null)
    Invoke-Sql "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('ARPPRODUCTICON', 'app.ico')"
    $db.GetType().InvokeMember('Commit', 'InvokeMethod', $null, $db, $null)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($db)
}
Set-MsiArpIcon (Join-Path $releases 'LazySF2Patch-win.msi') (Join-Path $PSScriptRoot 'assets\app.ico')

Get-ChildItem $releases | Select-Object Name, Length
