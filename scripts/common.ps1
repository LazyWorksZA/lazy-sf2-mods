# Shared helpers for install.ps1 / uninstall.ps1.
$ManifestName = 'lazysf2.manifest.txt'

function Resolve-GameDir([string]$GameDir) {
    if (-not $GameDir) {
        $key = 'HKLM:\SOFTWARE\WOW6432Node\ThirdWire\Strike Fighters 2'
        $exe = (Get-ItemProperty $key -ErrorAction SilentlyContinue).Path
        if (-not $exe) { throw "Game folder not found in the registry ($key); pass -GameDir." }
        $GameDir = Split-Path $exe
    }
    if (-not (Test-Path (Join-Path $GameDir 'StrikeFighters2.exe'))) {
        throw "StrikeFighters2.exe not found in $GameDir"
    }
    return $GameDir
}
