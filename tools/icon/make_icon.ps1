# Draws the FeedbackPlease icon (a crosshair on a harmonic-stack syllable) at every icon
# size and writes assets/FeedbackPlease.ico (embedded in the exe) and assets/icon_256.png.
# Run from anywhere:  powershell -NoProfile -ExecutionPolicy Bypass -File tools\icon\make_icon.ps1
# Geometry is in a 120 x 120 design space; small sizes use bolder, simpler variants.

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$assets = Join-Path $root 'assets'
New-Item -ItemType Directory -Force $assets | Out-Null

function Color([string]$hex, [double]$alpha = 1.0) {
    $c = [System.Drawing.ColorTranslator]::FromHtml($hex)
    return [System.Drawing.Color]::FromArgb([int](255 * $alpha), $c)
}

function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Draw-Icon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = $size / 120.0
    $g.ScaleTransform($s, $s)
    $tiny = $size -le 20   # two bars, heaviest lines
    $small = $size -le 32  # three bars, bold lines
    $medium = $size -le 64 # all bars, slightly heavier lines

    # Tile
    $bg = New-Object System.Drawing.SolidBrush (Color '#1b2333')
    $g.FillPath($bg, (RoundRect 0 0 120 120 ($(if ($small) { 22 } else { 26 }))))

    # Syllable: a harmonic stack, fading towards the top harmonics at the bottom
    $cyan = '#5ad1e6'
    if ($tiny) {
        $bars = @(@(38, 1.0), @(66, 0.8))
        $bh = 16; $bx = 40; $bw = 40
    } elseif ($small) {
        $bars = @(@(30, 1.0), @(52, 1.0), @(74, 0.7))
        $bh = 13; $bx = 42; $bw = 36
    } else {
        $bars = @(@(24, 1.0), @(38, 1.0), @(52, 1.0), @(66, 0.8), @(80, 0.6), @(94, 0.4))
        $bh = 7; $bx = 46; $bw = 28
    }
    foreach ($b in $bars) {
        $br = New-Object System.Drawing.SolidBrush (Color $cyan $b[1])
        $g.FillPath($br, (RoundRect $bx $b[0] $bw $bh ($bh / 2)))
    }

    # Crosshair
    $orange = Color '#f28c33'
    $w = $(if ($tiny) { 12 } elseif ($small) { 9 } elseif ($medium) { 6 } else { 4 })
    $pen = New-Object System.Drawing.Pen $orange, $w
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $r = $(if ($tiny) { 36 } elseif ($small) { 32 } else { 30 })
    $g.DrawEllipse($pen, 60 - $r, 60 - $r, 2 * $r, 2 * $r)
    $out = $(if ($tiny) { 10 } elseif ($small) { 12 } else { 18 }); $in = $(if ($tiny) { 20 } elseif ($small) { 22 } else { 34 })
    $g.DrawLine($pen, 60, $out, 60, $in)
    $g.DrawLine($pen, 60, 120 - $in, 60, 120 - $out)
    $g.DrawLine($pen, $out, 60, $in, 60)
    $g.DrawLine($pen, 120 - $in, 60, 120 - $out, 60)

    $g.Dispose()
    return $bmp
}

# Classic icon image: BITMAPINFOHEADER (height doubled for the mask), 32-bit BGRA rows
# bottom-up, then an all-zero AND mask (transparency comes from the alpha channel).
function Dib([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $px = New-Object byte[] ($data.Stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $px, 0, $px.Length)
    $bmp.UnlockBits($data)
    $maskRow = [int][Math]::Ceiling($w / 32.0) * 4
    $ms = New-Object System.IO.MemoryStream
    $wr = New-Object System.IO.BinaryWriter $ms
    $wr.Write([uint32]40); $wr.Write([int32]$w); $wr.Write([int32](2 * $h))
    $wr.Write([uint16]1); $wr.Write([uint16]32); $wr.Write([uint32]0)
    $wr.Write([uint32]($w * $h * 4 + $maskRow * $h))
    $wr.Write([int32]0); $wr.Write([int32]0); $wr.Write([uint32]0); $wr.Write([uint32]0)
    for ($y = $h - 1; $y -ge 0; $y--) { $wr.Write($px, $y * $data.Stride, $w * 4) }
    $wr.Write((New-Object byte[] ($maskRow * $h)))
    $wr.Flush()
    return , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$pngs = @()  # image data per size: PNG for 256 px (as Windows does), classic bitmaps below
foreach ($sz in $sizes) {
    $bmp = Draw-Icon $sz
    if ($sz -ge 256) {
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $pngs += , $ms.ToArray()
        $bmp.Save((Join-Path $assets 'icon_256.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    } else {
        $pngs += , (Dib $bmp)
    }
    $bmp.Dispose()
}

# ICO: header, one directory entry per size, then the PNG images.
$ico = New-Object System.IO.MemoryStream
$wr = New-Object System.IO.BinaryWriter $ico
$wr.Write([uint16]0); $wr.Write([uint16]1); $wr.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]
    $dim = $(if ($sz -ge 256) { 0 } else { $sz })
    $wr.Write([byte]$dim); $wr.Write([byte]$dim); $wr.Write([byte]0); $wr.Write([byte]0)
    $wr.Write([uint16]1); $wr.Write([uint16]32)
    $wr.Write([uint32]$pngs[$i].Length); $wr.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $wr.Write($p) }
$wr.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $assets 'FeedbackPlease.ico'), $ico.ToArray())
"Wrote $(Join-Path $assets 'FeedbackPlease.ico') ($($sizes -join ', ') px) and icon_256.png"
