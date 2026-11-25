<#
.SYNOPSIS
  Runs one of the Ghidra scripts in this folder, read-only, against an existing Ghidra project.

.EXAMPLE
  .\Invoke-Ghidra.ps1 -Folder /Core -Program GraphicsEngineDX10.dll -Script StringXrefs.java -ScriptArgs _DoFullScreen
  .\Invoke-Ghidra.ps1 -Program StrikeFighters2.exe -Script ImportXrefs.java -ScriptArgs CreateWindowExW

.NOTES
  - Output of the scripts (decompiled C) goes to -OutDir. Decompiled game code must never be committed.
  - Ghidra's .bat launcher breaks on paths containing "(x86)". Import the game through a path
    without parentheses, for example a junction: New-Item -ItemType Junction <path> -Target <game folder>
  - Ghidra 12.2 needs JDK 25 (JAVA_HOME).
  - The project folder and Ghidra's install are machine-specific: pass -ProjectDir and -GhidraHome, or set
    LAZYSF2_GHIDRA_PROJECTS and LAZYSF2_GHIDRA_HOME.
#>
param(
    [string]$Folder = '',
    [Parameter(Mandatory)] [string]$Program,
    [Parameter(Mandatory)] [string]$Script,
    [string[]]$ScriptArgs = @(),
    [string]$ProjectDir = $env:LAZYSF2_GHIDRA_PROJECTS,
    [string]$ProjectName = 'SF2',
    [string]$GhidraHome = $env:LAZYSF2_GHIDRA_HOME,
    [string]$OutDir = (Join-Path $env:TEMP 'sf2-ghidra-out')
)

if (-not $ProjectDir -or -not $GhidraHome) { throw 'Set -ProjectDir and -GhidraHome (or LAZYSF2_GHIDRA_PROJECTS and LAZYSF2_GHIDRA_HOME).' }
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = [Environment]::GetEnvironmentVariable('JAVA_HOME', 'Machine') }
New-Item -ItemType Directory -Force $OutDir | Out-Null

& (Join-Path $GhidraHome 'support\analyzeHeadless.bat') $ProjectDir "$ProjectName$Folder" `
    -process $Program -noanalysis -readOnly `
    -scriptPath $PSScriptRoot -postScript $Script $OutDir @ScriptArgs 2>&1 |
    Select-String 'XREF|STR |PROGRAM|ERROR|OUT ' |
    ForEach-Object { $_.Line -replace '^.*\.java> ', '' -replace ' \(GhidraScript\)\s*$', '' }
