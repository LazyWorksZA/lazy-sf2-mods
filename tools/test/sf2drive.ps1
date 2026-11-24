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
    [ValidateSet('start', 'focus', 'shot', 'away', 'stop')] [string]$Action,
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
    'stop' { Get-Process StrikeFighters2 -ErrorAction SilentlyContinue | ForEach-Object { [void]$_.CloseMainWindow(); if (-not $_.WaitForExit(15000)) { Stop-Process -Id $_.Id -Force; 'force-stopped' } else { 'stopped' } } }
}
