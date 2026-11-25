<#
.SYNOPSIS
  Builds installer\assets\app.ico from installer\assets\jet-fighter.svg (a single-path SVG), using WPF to draw
  the path at 16-256 px. The .ico holds PNG-compressed images.

.NOTES
  Icon: "Jet fighter" by Delapouite, https://game-icons.net/1x1/delapouite/jet-fighter.html, CC BY 3.0
  (see THIRD_PARTY_NOTICES.md). Recoloured to #e8a832 on a transparent background.
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore, WindowsBase
$assets = Join-Path $PSScriptRoot 'assets'
$svg = Get-Content (Join-Path $assets 'jet-fighter.svg') -Raw
if ($svg -notmatch 'fill="(#[0-9a-fA-F]{6})" d="([^"]+)"') { throw 'expected one <path fill=".." d=".."> in the SVG' }
$brush = [Windows.Media.SolidColorBrush][Windows.Media.ColorConverter]::ConvertFromString($Matches[1])
$geometry = [Windows.Media.Geometry]::Parse($Matches[2])

$sizes = 16, 24, 32, 48, 64, 128, 256
$images = foreach ($n in $sizes) {
    $visual = New-Object Windows.Media.DrawingVisual
    $dc = $visual.RenderOpen()
    $dc.PushTransform((New-Object Windows.Media.ScaleTransform ($n / 512.0), ($n / 512.0)))
    $dc.DrawGeometry($brush, $null, $geometry)
    $dc.Pop(); $dc.Close()
    $bmp = New-Object Windows.Media.Imaging.RenderTargetBitmap $n, $n, 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($visual)
    $enc = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bmp))
    $ms = New-Object IO.MemoryStream; $enc.Save($ms)
    , $ms.ToArray()
}

# ICONDIR, then one ICONDIRENTRY per image, then the PNG data.
$out = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $n = $sizes[$i]; $b = if ($n -ge 256) { 0 } else { $n }
    $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
[IO.File]::WriteAllBytes((Join-Path $assets 'app.ico'), $out.ToArray())
Write-Host "Wrote $(Join-Path $assets 'app.ico') ($($sizes -join ', ') px)"
