<#
.SYNOPSIS
  Drives SF2 for unattended checks: start, focus, screenshot, switch away and back, stop.

.DESCRIPTION
  A process started from a background shell isn't allowed to take the foreground, so SF2 waits
  "inactive". 'focus' uses a synthetic Alt key press to get past the foreground lock. 'away'
  focuses the taskbar so the game loses focus. 'shot' saves a half-size PNG of the game's monitor.
  This sends input to the desktop session, so use it only with the user's OK. When DxvkLogs is set,
  'start' turns on DXVK's log (written there) and HUD.

.EXAMPLE
  .\tools\test\sf2drive.ps1 start; .\tools\test\sf2drive.ps1 focus
  .\tools\test\sf2drive.ps1 shot -Out G:\SF2-recon\shot.png
#>
param(
    [ValidateSet('start', 'focus', 'shot', 'away', 'stop', 'click', 'key', 'fly', 'run')] [string]$Action,
    [double]$X = 0, [double]$Y = 0,
    [string]$Key = '',
    [int]$Seconds = 90,
    [string]$Shots = '',
    [string]$Out = '',
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
    $s = New-Object System.Drawing.Bitmap $b, ([int]($w / 2)), ([int]($h / 2))
    $s.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $s.Dispose(); $g.Dispose(); $b.Dispose()
}
[void][D]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-GameWindow {
    $p = Get-Process StrikeFighters2 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { throw 'game not running' }
    $p.Refresh(); return $p.MainWindowHandle
}

$scan = @{ esc = 0x01; enter = 0x1C; space = 0x39; tab = 0x0F; up = 0x48; down = 0x50; left = 0x4B; right = 0x4D
           f1 = 0x3B; f2 = 0x3C; f3 = 0x3D; f4 = 0x3E; f5 = 0x3F; f6 = 0x40; f7 = 0x41; f8 = 0x42; f9 = 0x43; f10 = 0x44
           y = 0x15; n = 0x31; w = 0x11; a = 0x1E; s = 0x1F; d = 0x20; b = 0x30; p = 0x19; backspace = 0x0E }
function Send-Key([string]$name) {
    $k = $name.ToLower()
    if (-not $scan.ContainsKey($k)) { throw "unknown key '$name'" }
    $c = [byte]$scan[$k]
    # KEYEVENTF_SCANCODE (8), plus KEYEVENTF_EXTENDEDKEY (1) for the arrow keys.
    $ext = if ($k -in 'up', 'down', 'left', 'right') { 1 } else { 0 }
    [D]::keybd_event(0, $c, (8 -bor $ext), [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    [D]::keybd_event(0, $c, (8 -bor $ext -bor 2), [UIntPtr]::Zero); Start-Sleep -Milliseconds 120
}
function Click-Virtual([double]$vx, [double]$vy) {
    $r = New-Object D+RECT; [void][D]::GetWindowRect((Get-GameWindow), [ref]$r)
    $sx = [int]($r.L + $vx * ($r.R - $r.L) / 1024); $sy = [int]($r.T + $vy * ($r.B - $r.T) / 768)
    [void][D]::SetCursorPos($sx - 6, $sy - 4); Start-Sleep -Milliseconds 120   # a small move first, so the game sees motion
    [void][D]::SetCursorPos($sx, $sy); Start-Sleep -Milliseconds 150
    [D]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    [D]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 200
}

switch ($Action) {
    'start' {
        if ($DxvkLogs) {
            $env:DXVK_LOG_PATH = $DxvkLogs
            $env:DXVK_HUD = 'devinfo,fps,api'
        }
        Start-Process "$game\StrikeFighters2.exe" -WorkingDirectory $game | Out-Null
        for ($i = 0; $i -lt 60; $i++) { Start-Sleep 1; $p = Get-Process StrikeFighters2 -ErrorAction SilentlyContinue; if ($p) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break } } }
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
        Save-Shot $scr.X $scr.Y $scr.Width $scr.Height $Out; "shot: $Out window=($($r.L),$($r.T))-($($r.R),$($r.B)) screen=$scr"
    }
    'click' { Click-Virtual $X $Y; "click: virtual ($X,$Y)" }
    'key' { Send-Key $Key; "key: $Key" }
    'stop' { Get-Process StrikeFighters2 -ErrorAction SilentlyContinue | ForEach-Object { [void]$_.CloseMainWindow(); if (-not $_.WaitForExit(15000)) { Stop-Process -Id $_.Id -Force; 'force-stopped' } else { 'stopped' } } }
}
