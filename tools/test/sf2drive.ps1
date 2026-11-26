<#
.SYNOPSIS
  Drives SF2 for unattended checks: start, focus, screenshot, switch away and back, stop, and a full flight.

.DESCRIPTION
  A process started from a background shell isn't allowed to take the foreground, so SF2 waits
  "inactive". 'focus' uses a synthetic Alt key press to get past the foreground lock. 'away'
  focuses the taskbar so the game loses focus. 'shot' saves a full-size PNG of the game's monitor, plus <name>-small.png at 1280 px wide.
  'key' takes a key or a chord (shift+f5, ctrl+shift+t): modifiers go down in order, then the key, then release in reverse.
  'hold' holds a chord down for -Ms (default 1500) and, with -Out, takes the screenshot while it is held.
  Key names: a-z, 0-9, f1-f12, numpad0-numpad9, numpadperiod, lshift, lctrl, lalt (shift, ctrl, alt are aliases), esc, enter, space, tab, arrows, backspace.
  This sends input to the desktop session, so use it only with the user's OK. When DxvkLogs is set,
  'start' turns on DXVK's log (written there) and HUD.

  'fly' goes from the main menu to Single Mission, sets Mission Type (-MissionType, default Strike),
  Accepts, clicks Fly! in the hangar, waits -Seconds in the cockpit, presses Esc (which goes straight to
  the debrief), clicks Accept and Exit, and waits for the main menu. (The game starts the flight in the air.) 'run' is start + fly + EXIT on the main menu, and prints the new lazysf2.log lines.
  Screens are recognised by sampling the menus' tan text colour; each wait has a timeout, and on any
  failure 'run' stops the game it started (killing it if it won't close). start and run refuse to touch a game that is already running unless -Force. With -Shots DIR, each stage is saved there.

.EXAMPLE
  .\tools\test\sf2drive.ps1 start; .\tools\test\sf2drive.ps1 focus
  .\tools\test\sf2drive.ps1 shot -Out <scratch>\shot.png               # also writes shot-small.png
  .\tools\test\sf2drive.ps1 key -Key ctrl+shift+t
  .\tools\test\sf2drive.ps1 hold -Key numpad4 -Ms 2000 -Out <scratch>\held.png
  .\tools\test\sf2drive.ps1 run -Seconds 30 -Shots <scratch>\drive   # launch, fly a Strike mission 30 s, debrief, quit; prints the log lines
  .\tools\test\sf2drive.ps1 fly -Seconds 30                             # from the main menu, back to the main menu
#>
param(
    [ValidateSet('start', 'focus', 'shot', 'away', 'stop', 'click', 'key', 'hold', 'fly', 'run')] [string]$Action,
    [double]$X = 0, [double]$Y = 0,
    [string]$Key = '',
    [int]$Ms = 1500,   # hold: how long the chord stays down
    [int]$Seconds = 90,
    [ValidateSet('Strike', 'CloseAirSupport', 'AirDefenseSuppression', 'ArmedRecon', 'AntiShipping', 'Recon')] [string]$MissionType = 'Strike',
    [string]$Shots = '',
    [string]$Out = '',
    [switch]$Force,   # start/run: close a game that is already running (it may be the user's) instead of refusing
    [string]$GameDir = (Split-Path (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\ThirdWire\Strike Fighters 2').Path),
    [string]$DxvkLogs = ''
)
$ErrorActionPreference = 'Stop'
$game = $GameDir
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class D {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  // A synthetic Alt press lets this (background) process pass the foreground lock.
  public static bool Focus(IntPtr h) {
    keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero);
    return SetForegroundWindow(h);
  }
}
'@

function Save-Shot($x, $y, $w, $h, $path) {
    $b = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $b.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $small = Join-Path (Split-Path $path) (([System.IO.Path]::GetFileNameWithoutExtension($path)) + '-small.png')
    $sh = [int]($h * 1280 / $w)
    $s = New-Object System.Drawing.Bitmap 1280, $sh
    $sg = [System.Drawing.Graphics]::FromImage($s)
    $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $sg.DrawImage($b, 0, 0, 1280, $sh); $sg.Dispose()
    $s.Save($small, [System.Drawing.Imaging.ImageFormat]::Png)
    $s.Dispose(); $g.Dispose(); $b.Dispose()
    return $small
}
[void][D]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-GameWindow {
    $p = Get-Process StrikeFighters2 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { throw 'game not running' }
    $p.Refresh(); return $p.MainWindowHandle
}

$scan = @{ esc = 0x01; enter = 0x1C; space = 0x39; tab = 0x0F; up = 0x48; down = 0x50; left = 0x4B; right = 0x4D; backspace = 0x0E
           f1 = 0x3B; f2 = 0x3C; f3 = 0x3D; f4 = 0x3E; f5 = 0x3F; f6 = 0x40; f7 = 0x41; f8 = 0x42; f9 = 0x43; f10 = 0x44; f11 = 0x57; f12 = 0x58
           lshift = 0x2A; lctrl = 0x1D; lalt = 0x38; numpadperiod = 0x53
           '1' = 0x02; '2' = 0x03; '3' = 0x04; '4' = 0x05; '5' = 0x06; '6' = 0x07; '7' = 0x08; '8' = 0x09; '9' = 0x0A; '0' = 0x0B
           q = 0x10; w = 0x11; e = 0x12; r = 0x13; t = 0x14; y = 0x15; u = 0x16; i = 0x17; o = 0x18; p = 0x19
           a = 0x1E; s = 0x1F; d = 0x20; f = 0x21; g = 0x22; h = 0x23; j = 0x24; k = 0x25; l = 0x26
           z = 0x2C; x = 0x2D; c = 0x2E; v = 0x2F; b = 0x30; n = 0x31; m = 0x32 }
# The numpad digits are not extended keys: numpad0..9 = 0x52 0x4F 0x50 0x51 0x4B 0x4C 0x4D 0x47 0x48 0x49.
$np = 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49
for ($i = 0; $i -lt 10; $i++) { $scan["numpad$i"] = $np[$i] }
$alias = @{ shift = 'lshift'; ctrl = 'lctrl'; alt = 'lalt' }
function Get-KeyCode([string]$name) {
    $k = $name.Trim().ToLower()
    if ($alias.ContainsKey($k)) { $k = $alias[$k] }
    if (-not $scan.ContainsKey($k)) { throw "unknown key '$name'" }
    return $k
}
function Send-KeyEvent([string]$k, [bool]$up) {
    # KEYEVENTF_SCANCODE (8), plus KEYEVENTF_EXTENDEDKEY (1) for the arrow keys, plus KEYEVENTF_KEYUP (2).
    $ext = if ($k -in 'up', 'down', 'left', 'right') { 1 } else { 0 }
    $f = 8 -bor $ext; if ($up) { $f = $f -bor 2 }
    [D]::keybd_event(0, [byte]$scan[$k], $f, [UIntPtr]::Zero)
}
function Get-Chord([string]$chord) { @($chord -split '\+' | ForEach-Object { Get-KeyCode $_ }) }
function Press-Chord([string[]]$keys) { foreach ($k in $keys) { Send-KeyEvent $k $false; Start-Sleep -Milliseconds 40 } }
function Release-Chord([string[]]$keys) { for ($i = $keys.Count - 1; $i -ge 0; $i--) { Send-KeyEvent $keys[$i] $true; Start-Sleep -Milliseconds 40 } }
function Send-Key([string]$name) {
    $keys = Get-Chord $name
    Press-Chord $keys; Start-Sleep -Milliseconds 80
    Release-Chord $keys; Start-Sleep -Milliseconds 120
}
function Click-Virtual([double]$vx, [double]$vy) {
    $r = New-Object D+RECT; [void][D]::GetWindowRect((Get-GameWindow), [ref]$r)
    $sx = [int]($r.L + $vx * ($r.R - $r.L) / 1024); $sy = [int]($r.T + $vy * ($r.B - $r.T) / 768)
    [void][D]::SetCursorPos($sx - 6, $sy - 4); Start-Sleep -Milliseconds 120   # a small move first, so the game sees motion
    [void][D]::SetCursorPos($sx, $sy); Start-Sleep -Milliseconds 150
    [D]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    [D]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 200
}

# ---- screen detection and the flight driver -------------------------------------------------
$logPath = Join-Path $game 'lazysf2.log'
function Get-LogLength { if (Test-Path $logPath) { (Get-Item $logPath).Length } else { 0 } }
function Read-LogFrom([long]$from) {
    if (-not (Test-Path $logPath)) { return @() }
    $fs = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
    try {
        if ($from -gt $fs.Length) { $from = 0 }
        [void]$fs.Seek($from, 'Begin')
        (New-Object System.IO.StreamReader($fs)).ReadToEnd() -split "`r?`n" | Where-Object { $_ }
    } finally { $fs.Dispose() }
}
function Get-Frame {   # the game's monitor as a 1280x720 bitmap (the same space as the half-size shots)
    $scr = [System.Windows.Forms.Screen]::FromHandle((Get-GameWindow)).Bounds
    $b = New-Object System.Drawing.Bitmap $scr.Width, $scr.Height
    $g = [System.Drawing.Graphics]::FromImage($b); $g.CopyFromScreen($scr.X, $scr.Y, 0, 0, $scr.Size); $g.Dispose()
    $s = New-Object System.Drawing.Bitmap $b, 1280, 720; $b.Dispose(); return $s
}
function Count-Tan($bmp, $x0, $y0, $x1, $y1) {   # pixels of the menus' tan text colour in a 1280x720 rectangle
    $n = 0
    for ($y = $y0; $y -lt $y1; $y++) { for ($x = $x0; $x -lt $x1; $x++) {
        $c = $bmp.GetPixel($x, $y)
        if ($c.R -gt 170 -and $c.G -gt 160 -and $c.B -gt 80 -and $c.B -lt 170 -and ($c.R - $c.B) -gt 50 -and [math]::Abs($c.R - $c.G) -lt 25) { $n++ } } }
    $n
}
function Get-Screen {   # 'main', 'single', 'hangar', 'debrief' or 'other'
    $f = Get-Frame
    try {
        if ((Count-Tan $f 850 215 1130 260) -gt 150) { return 'main' }       # "Instant Action"
        if ((Count-Tan $f 1040 100 1165 125) -gt 60) { return 'single' }     # "Load Mission" tab
        if ((Count-Tan $f 100 565 180 588) -gt 60) { return 'hangar' }       # "Loadout"
        if ((Count-Tan $f 520 100 580 125) -gt 8 -or (Count-Tan $f 55 640 110 662) -gt 30) { return 'debrief' }   # "Stats" tab or "Refly" (Exit shares Refly's spot, but its screens match first)
        return 'other'
    } finally { $f.Dispose() }
}
function Save-Stage([string]$dir, [string]$name) {
    if (-not $dir) { return }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $scr = [System.Windows.Forms.Screen]::FromHandle((Get-GameWindow)).Bounds
    [void](Save-Shot $scr.X $scr.Y $scr.Width $scr.Height (Join-Path $dir "$name.png"))
}
function Wait-Screen([string]$want, [int]$timeout, [string]$dir, [string]$name) {
    $end = (Get-Date).AddSeconds($timeout)
    while ((Get-Date) -lt $end) {
        if (-not (Get-Process StrikeFighters2 -ErrorAction SilentlyContinue)) { throw "game exited while waiting for '$want'" }
        if ((Get-Screen) -eq $want) { Start-Sleep -Milliseconds 500; Save-Stage $dir $name; return }
        Start-Sleep -Milliseconds 700
    }
    Save-Stage $dir "$name-TIMEOUT"; throw "timed out after ${timeout}s waiting for the '$want' screen"
}
# Awards (e.g. a Purple Heart from an earlier flight) show one per visit on the way to the Single Mission screen;
# a click dismisses each. Click through them for up to 60 s until the Single Mission screen shows.
function Skip-Awards {
    $end = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $end -and (Get-Screen) -ne 'single') {
        Start-Sleep -Seconds 2
        if ((Get-Screen) -eq 'other') { Click-Virtual 512 700 }
    }
}
function Fly-Mission([int]$secs, [string]$dir, [string]$type) {
    [void][D]::Focus((Get-GameWindow))
    Wait-Screen 'main' 90 $dir '1-main'
    $mark = Get-LogLength
    Click-Virtual 790 330   # Single Mission
    Skip-Awards
    Wait-Screen 'single' 30 $dir '2-single-mission'
    Click-Virtual 653 120   # Create Mission tab
    # Mission Type dropdown: open it, then click the wanted row (rows are 15 px apart in the 1280x720 space).
    $types = 'Strike', 'CloseAirSupport', 'AirDefenseSuppression', 'ArmedRecon', 'AntiShipping', 'Recon'
    Click-Virtual 868 310
    Click-Virtual 800 ((313 + 15 * [array]::IndexOf($types, $type)) * 768 / 720)
    Start-Sleep -Milliseconds 500; Save-Stage $dir '3-mission-type'
    Click-Virtual 976 694   # Accept
    Wait-Screen 'hangar' 60 $dir '4-hangar'
    Click-Virtual 960 694   # Fly!
    # The Tacview line marks the flight starting; without it (recording off) fall back to a fixed wait.
    $end = (Get-Date).AddSeconds(180); $started = $false
    while ((Get-Date) -lt $end) {
        if ((Read-LogFrom $mark) -match 'tacview: recording to') { $started = $true; break }
        if (-not (Get-Process StrikeFighters2 -ErrorAction SilentlyContinue)) { throw 'game exited while loading' }
        Start-Sleep -Milliseconds 700
    }
    if (-not $started) { Write-Warning 'no "tacview: recording to" line within 180 s; assuming the flight started' }
    Start-Sleep -Seconds 2; Save-Stage $dir '5-flight-start'
    Start-Sleep -Seconds $secs; Save-Stage $dir '6-flight-end'
    Send-Key esc   # in flight Esc goes straight to the debrief
    Wait-Screen 'debrief' 40 $dir '7-debrief'
    Click-Virtual 950 695   # Accept: back to the Single Mission screen
    Skip-Awards
    Wait-Screen 'single' 40 $dir '8-single-mission'
    Click-Virtual 48 694    # Exit: back to the main menu
    Wait-Screen 'main' 40 $dir '9-back-at-main'
}
function Stop-Game([int]$OnlyId = 0) {
    Get-Process StrikeFighters2 -ErrorAction SilentlyContinue | Where-Object { -not $OnlyId -or $_.Id -eq $OnlyId } | ForEach-Object {
        [void]$_.CloseMainWindow()
        if (-not $_.WaitForExit(15000)) { Stop-Process -Id $_.Id -Force; 'force-stopped' } else { 'stopped' } }
}
function Assert-NoGame {   # never touch a game this script did not start, unless -Force
    $p = Get-Process StrikeFighters2 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { return }
    if (-not $Force) { throw "a game is already running (pid $($p.Id), started $($p.StartTime)); close it, or pass -Force" }
    Stop-Game | Out-Null
}
function Start-Game {
    Assert-NoGame
    Start-Process "$game\StrikeFighters2.exe" -WorkingDirectory $game | Out-Null
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep 1; $p = Get-Process StrikeFighters2 -ErrorAction SilentlyContinue
        if ($p) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break } }
    }
}

switch ($Action) {
    'start' {
        if ($DxvkLogs) {
            $env:DXVK_LOG_PATH = $DxvkLogs
            $env:DXVK_HUD = 'devinfo,fps,api'
        }
        Start-Game
        "started, window=$((Get-GameWindow))"
    }
    'focus' { $h = Get-GameWindow; "focus: $([D]::Focus($h)), foreground is game: $([D]::GetForegroundWindow() -eq $h)" }
    'away' {
        # Focus the taskbar (a harmless, always-present window), so the game loses focus.
        $tray = [D]::FindWindow('Shell_TrayWnd', $null)
        "away: $([D]::Focus($tray)), foreground is game: $([D]::GetForegroundWindow() -eq (Get-GameWindow))"
    }
    'shot' {
        $h = Get-GameWindow; $r = New-Object D+RECT; [void][D]::GetWindowRect($h, [ref]$r)
        # Capture the monitor the game is on (fall back to the primary if minimised off-screen).
        $scr = [System.Windows.Forms.Screen]::FromHandle($h).Bounds
        $small = Save-Shot $scr.X $scr.Y $scr.Width $scr.Height $Out; "shot: $Out, $small window=($($r.L),$($r.T))-($($r.R),$($r.B)) screen=$scr"
    }
    'click' { [void][D]::Focus((Get-GameWindow)); Click-Virtual $X $Y; "click: virtual ($X,$Y)" }
    'key' { [void][D]::Focus((Get-GameWindow)); Send-Key $Key; "key: $Key" }
    'hold' {
        [void][D]::Focus((Get-GameWindow))
        $keys = Get-Chord $Key
        $small = ''
        Press-Chord $keys
        try {
            if ($Out) {
                Start-Sleep -Milliseconds ([math]::Max(0, $Ms - 300))
                $scr = [System.Windows.Forms.Screen]::FromHandle((Get-GameWindow)).Bounds
                $small = Save-Shot $scr.X $scr.Y $scr.Width $scr.Height $Out
                Start-Sleep -Milliseconds 300
            } else { Start-Sleep -Milliseconds $Ms }
        } finally { Release-Chord $keys }
        "hold: $Key for $Ms ms" + $(if ($Out) { ", shot: $Out, $small" } else { '' })
    }
    'stop' { Stop-Game }
    'fly' { Fly-Mission $Seconds $Shots $MissionType; 'fly: done, back at the main menu' }
    'run' {
        $ok = $false
        $mark = Get-LogLength
        $mine = 0
        try {
            Start-Game
            $mine = (Get-Process StrikeFighters2 -ErrorAction SilentlyContinue | Select-Object -First 1).Id
            Fly-Mission $Seconds $Shots $MissionType
            Click-Virtual 894 654   # EXIT on the main menu
            $p = Get-Process StrikeFighters2 -ErrorAction SilentlyContinue
            if ($p -and -not $p.WaitForExit(20000)) { Save-Stage $Shots '10-quit-TIMEOUT'; throw 'game did not quit after EXIT' }
            $ok = $true; 'run: done, the game quit from the main menu'
        }
        catch { Write-Warning "run failed: $_" }
        finally { if ($mine) { $r = Stop-Game $mine; if ($r) { "cleanup: $r" } } }
        '--- new lazysf2.log lines ---'
        Read-LogFrom $mark
        if (-not $ok) { exit 1 }
    }
}
