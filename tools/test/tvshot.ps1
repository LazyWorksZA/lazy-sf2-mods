<#
.SYNOPSIS
  Opens a Tacview recording and captures zoom-level screenshots of the Tacview window.

.DESCRIPTION
  Refuses to run (exit 2) when Tacview is already running, because its owner may be using it.
  Starts Tacview with the recording, waits until it is loaded (CPU idle for about 2 s, at most 90 s),
  brings its window to the front, puts the cursor at the window centre and sends mouse-wheel notches.
  After each step it waits -Settle seconds and captures only the Tacview window, as <name>.png
  (full size) and <name>-small.png (1280 px wide). Closes Tacview gracefully unless -Keep.
  Finally prints the last LazySF2Patch/LazyTile/ERR lines from the newest Tacview trace log
  (%TEMP%\Tacview\Tacview-Trace-*.log), where add-on output goes.
  Sends input to the desktop session, so use it only with the user's OK.

.EXAMPLE
  .\tools\test\tvshot.ps1 -Recording <scratch>\flight.acmi -Out <scratch>\tv
  .\tools\test\tvshot.ps1 -Recording <file> -Out <dir> -Zoom 8,8,8 -ZoomOut 20 -Keep
#>
param(
    [Parameter(Mandatory)] [string]$Recording,
    [Parameter(Mandatory)] [string]$Out,
    [int[]]$Zoom = @(12, 4, 4, 4, 4, 4),   # wheel notches per step (positive zooms in)
    [int]$ZoomOut = 0,                      # notches back out at the end, then one more shot
    [string]$Tacview = '',
    [int]$Settle = 5,
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not ('TvShot.W' -as [type])) {
    Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
namespace TvShot {
  public static class W {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, int d, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    // A synthetic Alt press lets a background process pass the foreground lock.
    public static bool Focus(IntPtr h) {
      if (IsIconic(h)) ShowWindow(h, 9);
      keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero);
      return SetForegroundWindow(h);
    }
    public static void Wheel(int notches) {
      int step = notches < 0 ? -120 : 120;
      for (int i = 0; i < Math.Abs(notches); i++) { mouse_event(0x0800, 0, 0, step, UIntPtr.Zero); System.Threading.Thread.Sleep(150); }
    }
  }
}
'@
}
[void][TvShot.W]::SetProcessDpiAwarenessContext([IntPtr](-4))

if (Get-Process -Name 'Tacview*' -ErrorAction SilentlyContinue) {
    Write-Output 'tvshot: Tacview is already running (the owner may be using it); close it first. Nothing done.'
    exit 2
}
if (-not (Test-Path -LiteralPath $Recording)) { throw "recording not found: $Recording" }
if (-not $Tacview) {
    $Tacview = Get-ChildItem 'C:\Program Files*\Tacview*\Tacview64.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    if (-not $Tacview) { throw 'Tacview64.exe not found; pass -Tacview <exe>' }
}
New-Item -ItemType Directory -Force $Out | Out-Null

function Save-Window([IntPtr]$h, [string]$name) {
    $r = New-Object TvShot.W+RECT; [void][TvShot.W]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $full = Join-Path $Out "$name.png"; $small = Join-Path $Out "$name-small.png"
    $b = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht)); $g.Dispose()
    $b.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
    $sh = [int]($ht * 1280 / $w)
    $s = New-Object System.Drawing.Bitmap 1280, $sh
    $sg = [System.Drawing.Graphics]::FromImage($s)
    $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $sg.DrawImage($b, 0, 0, 1280, $sh); $sg.Dispose()
    $s.Save($small, [System.Drawing.Imaging.ImageFormat]::Png)
    $s.Dispose(); $b.Dispose()
    "shot ${name}: $full  $small"
}

$mine = $null
try {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $mine = Start-Process -FilePath $Tacview -ArgumentList "`"$Recording`"" -PassThru
    # Loaded = a main window exists and the CPU has been idle for about 2 s (max 90 s).
    $prev = $null; $idle = 0; $last = [DateTime]::UtcNow
    while ($sw.Elapsed.TotalSeconds -lt 90) {
        Start-Sleep -Milliseconds 500
        $mine.Refresh()
        if ($mine.HasExited) { throw 'Tacview exited while loading' }
        $now = [DateTime]::UtcNow; $cpu = $mine.TotalProcessorTime.TotalSeconds
        if ($mine.MainWindowHandle -ne 0 -and $null -ne $prev) {
            $dt = ($now - $last).TotalSeconds
            if ((($cpu - $prev) / $dt) -lt 0.05) { $idle += $dt } else { $idle = 0 }
            if ($idle -ge 2) { break }
        }
        $prev = $cpu; $last = $now
    }
    $mine.Refresh()
    'open: {0:N0} s, mem {1:N0} MB' -f $sw.Elapsed.TotalSeconds, ($mine.WorkingSet64 / 1MB)

    $h = $mine.MainWindowHandle
    [void][TvShot.W]::Focus($h); Start-Sleep -Milliseconds 500
    $r = New-Object TvShot.W+RECT; [void][TvShot.W]::GetWindowRect($h, [ref]$r)
    [void][TvShot.W]::SetCursorPos([int](($r.L + $r.R) / 2), [int](($r.T + $r.B) / 2)); Start-Sleep -Milliseconds 300

    Start-Sleep -Seconds $Settle
    Save-Window $h 'zoom-00'
    $n = 0
    foreach ($z in $Zoom) {
        $n++
        [TvShot.W]::Wheel($z); Start-Sleep -Seconds $Settle
        Save-Window $h ('zoom-{0:00}' -f $n)
    }
    if ($ZoomOut -gt 0) {
        [TvShot.W]::Wheel(-$ZoomOut); Start-Sleep -Seconds $Settle
        Save-Window $h 'zoom-out'
    }
}
finally {
    if ($mine -and -not $mine.HasExited -and -not $Keep) {
        [void]$mine.CloseMainWindow()
        if (-not $mine.WaitForExit(15000)) { Write-Warning 'Tacview did not close within 15 s; left running' }
    }
    $log = Get-ChildItem (Join-Path $env:TEMP 'Tacview\Tacview-Trace-*.log') -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($log) {
        "--- $($log.Name): last LazySF2Patch|LazyTile|ERR lines ---"
        $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
        try { $text = (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
        $text -split "`r?`n" | Where-Object { $_ -match 'LazySF2Patch|LazyTile|ERR' } | Select-Object -Last 10
    } else { '--- no Tacview trace log found ---' }
}
