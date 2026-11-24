<#
.SYNOPSIS
  Runs one of the Ghidra scripts in this folder, read-only, against an existing Ghidra project.

.EXAMPLE
  .\Invoke-Ghidra.ps1 -Folder /Core -Program GraphicsEngineDX10.dll -Script StringXrefs.java -ScriptArgs _DoFullScreen
  .\Invoke-Ghidra.ps1 -Program StrikeFighters2.exe -Script ImportXrefs.java -ScriptArgs CreateWindowExW

.NOTES
  - Output of the scripts (decompiled C) goes to -OutDir. Decompiled game code must never be committed.
  - Ghidra's .bat launcher breaks on paths containing "(x86)". Import the game through a path
    without parentheses, for example a junction: New-Item -ItemType Junction G:\SF2-game -Target <game folder>
  - Ghidra 12.2 needs JDK 25 (JAVA_HOME).
#>
param(
    [string]$Folder = '',
    [Parameter(Mandatory)] [string]$Program,
    [Parameter(Mandatory)] [string]$Script,
    [string[]]$ScriptArgs = @(),
    [string]$ProjectDir = 'G:\GhidraProjects',
    [string]$ProjectName = 'SF2',
    [string]$GhidraHome = 'G:\DevTools\ghidra_12.2_PUBLIC',
    [string]$OutDir = (Join-Path $env:TEMP 'sf2-ghidra-out')
)

if (-not $env:JAVA_HOME) { $env:JAVA_HOME = [Environment]::GetEnvironmentVariable('JAVA_HOME', 'Machine') }
New-Item -ItemType Directory -Force $OutDir | Out-Null

& (Join-Path $GhidraHome 'support\analyzeHeadless.bat') $ProjectDir "$ProjectName$Folder" `
    -process $Program -noanalysis -readOnly `
    -scriptPath $PSScriptRoot -postScript $Script $OutDir @ScriptArgs 2>&1 |
    Select-String 'XREF|STR |PROGRAM|ERROR|OUT ' |
    ForEach-Object { $_.Line -replace '^.*\.java> ', '' -replace ' \(GhidraScript\)\s*$', '' }
