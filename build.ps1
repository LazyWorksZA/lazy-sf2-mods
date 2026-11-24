<#
.SYNOPSIS
  One-command x86 build. Produces build\<Config>\winmm.dll.

.EXAMPLE
  .\build.ps1             # Release
  .\build.ps1 -Config Debug
#>
param([ValidateSet('Release', 'Debug')] [string]$Config = 'Release')
$ErrorActionPreference = 'Stop'

# Enter the Visual Studio developer shell (x86 target) unless we are already in one.
if (-not $env:VCPKG_ROOT -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ x86/x64 toolset was not found.' }
    $env:Path = "$(Split-Path $vswhere);$env:Path"   # Launch-VsDevShell needs vswhere on PATH
    & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch x86 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}

Push-Location $PSScriptRoot
try {
    cmake --preset x86
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset "x86-$($Config.ToLower())"
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
    Write-Host "Built: $(Join-Path $PSScriptRoot "build\$Config\winmm.dll")"
} finally {
    Pop-Location
}
