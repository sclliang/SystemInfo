# Recreate the original vector-style application icon using Windows drawing APIs.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$iconPath = Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\app.ico'
$frames = [System.Collections.Generic.List[byte[]]]::new()
$sizes = @(16, 24, 32, 48, 64, 128, 256)
foreach ($size in $sizes) {
    $bitmap = [Drawing.Bitmap]::new($size, $size)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($size / 64.0, $size / 64.0)
    $graphics.Clear([Drawing.Color]::Transparent)
    $background = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(17, 77, 93))
    $paper = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(246, 250, 252))
    $line = [Drawing.Pen]::new([Drawing.Color]::FromArgb(53, 210, 190), 3.8)
    $line.StartCap = [Drawing.Drawing2D.LineCap]::Round
    $line.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $line.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $graphics.FillRectangle($background, 2, 2, 60, 60)
    $graphics.FillRectangle($paper, 15, 10, 34, 44)
    $thin = [Drawing.Pen]::new([Drawing.Color]::FromArgb(175, 196, 203), 2.5)
    $graphics.DrawLine($thin, 22, 19, 41, 19)
    $graphics.DrawLine($thin, 22, 25, 37, 25)
    $graphics.FillRectangle($background, 8, 31, 48, 16)
    $points = [Drawing.PointF[]]@([Drawing.PointF]::new(10, 40), [Drawing.PointF]::new(23, 40),
        [Drawing.PointF]::new(28, 33), [Drawing.PointF]::new(33, 46), [Drawing.PointF]::new(39, 37),
        [Drawing.PointF]::new(44, 40), [Drawing.PointF]::new(54, 40))
    $graphics.DrawLines($line, $points)
    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
    $frames.Add($stream.ToArray())
    $stream.Dispose(); $thin.Dispose(); $line.Dispose(); $paper.Dispose(); $background.Dispose()
    $graphics.Dispose(); $bitmap.Dispose()
}
$output = [IO.File]::Create($iconPath)
$writer = [IO.BinaryWriter]::new($output)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write($frame) }
} finally { $writer.Dispose(); $output.Dispose() }
Write-Host "Created: $iconPath"
