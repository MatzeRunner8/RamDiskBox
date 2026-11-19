# Erzeugt src\gui\app.ico (RAM-Modul auf blauem Grund) in mehreren Groessen als PNG-ICO.
param([string]$Out = "$PSScriptRoot\..\src\gui\app.ico")
Add-Type -AssemblyName System.Drawing

function New-RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconBitmap([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $k = $s / 256.0

    # Hintergrund: abgerundetes Quadrat mit Verlauf
    $bg = New-RoundRect (8*$k) (8*$k) (240*$k) (240*$k) (44*$k)
    $rect = New-Object System.Drawing.RectangleF (8*$k), (8*$k), (240*$k), (240*$k)
    $lg = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 38, 120, 220)), ([System.Drawing.Color]::FromArgb(255, 18, 52, 120)), 90
    $g.FillPath($lg, $bg)

    # RAM-Modul (Platine)
    $pcb = New-RoundRect (26*$k) (78*$k) (204*$k) (92*$k) (8*$k)
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 30, 150, 80))), $pcb)
    $g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 14, 90, 46)), ([float](4*$k))), $pcb)
    # Kerbe unten
    $g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 25, 80, 170))), (118*$k), (156*$k), (12*$k), (16*$k))

    # Chips
    $chipBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 28, 30, 36))
    foreach ($i in 0..3) {
        $x = (38 + $i * 48) * $k
        $chip = New-RoundRect $x (90*$k) (36*$k) (46*$k) (4*$k)
        $g.FillPath($chipBrush, $chip)
    }

    # Goldkontakte
    $gold = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 240, 190, 60))
    if ($s -ge 32) {
        $n = 14
        for ($i = 0; $i -lt $n; $i++) {
            $x = (34 + $i * 13.7) * $k
            if ($x -gt 112*$k -and $x -lt 134*$k) { continue }
            $g.FillRectangle($gold, $x, (146*$k), (8*$k), (16*$k))
        }
    } else {
        $g.FillRectangle($gold, (32*$k), (146*$k), (84*$k), (16*$k))
        $g.FillRectangle($gold, (134*$k), (146*$k), (90*$k), (16*$k))
    }

    # Blitz (schnell) unten rechts
    if ($s -ge 24) {
        $pts = @(
            (New-Object System.Drawing.PointF (180*$k), (176*$k)),
            (New-Object System.Drawing.PointF (152*$k), (218*$k)),
            (New-Object System.Drawing.PointF (172*$k), (218*$k)),
            (New-Object System.Drawing.PointF (160*$k), (248*$k)),
            (New-Object System.Drawing.PointF (200*$k), (200*$k)),
            (New-Object System.Drawing.PointF (180*$k), (200*$k)),
            (New-Object System.Drawing.PointF (194*$k), (176*$k))
        )
        $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 214, 64))), $pts)
        $g.DrawPolygon((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 150, 100, 0)), ([float](3*$k))), $pts)
    }
    $g.Dispose()
    return $bmp
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$images = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += , $ms.ToArray()
    $bmp.Dispose()
}

$fs = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $b = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$b); $bw.Write([byte]$b); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$images[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Flush()
$full = [System.IO.Path]::GetFullPath($Out)
[System.IO.File]::WriteAllBytes($full, $fs.ToArray())
Write-Output "Icon geschrieben: $full"
