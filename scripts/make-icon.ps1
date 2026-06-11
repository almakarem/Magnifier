# scripts/make-icon.ps1
# ---------------------------------------------------------------------------
# Converts app/icon.png (any reasonably square PNG, ideally >= 256x256) into
# a multi-resolution .ico at app/Magnifier.ico that Windows can use for the
# .exe icon, the tray icon, and the Start menu / Add-Remove-Programs entry.
#
# Generated sizes: 16, 24, 32, 48, 64, 128, 256 px. The 256 entry is stored
# as a PNG inside the .ico (Vista+ convention); smaller entries are stored
# as classic BMP DIB blobs so XP/legacy still works.
#
# Usage:
#   1. Save the source artwork as app/icon.png (square, transparent
#      background, at least 256x256).
#   2. From a PowerShell prompt at the repo root, run:
#          pwsh scripts/make-icon.ps1
#      (or "powershell -ExecutionPolicy Bypass -File scripts/make-icon.ps1"
#      if pwsh is not available).
#   3. Re-run CMake configure (CMake detects the .ico and adds it to the
#      build) and rebuild Magnifier.exe.
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot  = Split-Path -Parent $ScriptDir
$SrcPng    = Join-Path $RepoRoot 'app/icon.png'
$DstIco    = Join-Path $RepoRoot 'app/Magnifier.ico'

if (-not (Test-Path $SrcPng)) {
    Write-Error "Source not found: $SrcPng`nSave your icon artwork as 'app/icon.png' first."
}

Add-Type -AssemblyName System.Drawing

$source = [System.Drawing.Image]::FromFile($SrcPng)
try {
    $sizes = @(16, 24, 32, 48, 64, 128, 256)
    $resized = @()
    foreach ($s in $sizes) {
        $bm = New-Object System.Drawing.Bitmap $s, $s,
            ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g  = [System.Drawing.Graphics]::FromImage($bm)
        $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.DrawImage($source, 0, 0, $s, $s)
        $g.Dispose()
        $resized += [pscustomobject]@{ Size = $s; Bitmap = $bm }
    }

    # Encode each entry as PNG. Writing every entry as PNG is supported by
    # Vista+ and gives the smallest file at high quality. (Windows 7 and
    # later use these entries directly for high-DPI scaling.)
    $entries = @()
    foreach ($r in $resized) {
        $ms = New-Object System.IO.MemoryStream
        $r.Bitmap.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bytes = $ms.ToArray()
        $ms.Dispose()
        $entries += [pscustomobject]@{ Size = $r.Size; Bytes = $bytes }
    }

    # Build ICO container.
    $out = New-Object System.IO.MemoryStream
    $bw  = New-Object System.IO.BinaryWriter $out

    # ICONDIR
    $bw.Write([uint16]0)                # Reserved
    $bw.Write([uint16]1)                # Type 1 = icon
    $bw.Write([uint16]$entries.Count)   # Image count

    # 6-byte ICONDIR + 16-byte per entry directory; pixel data starts after.
    $offset = 6 + (16 * $entries.Count)
    foreach ($e in $entries) {
        $w = if ($e.Size -ge 256) { 0 } else { [byte]$e.Size }
        $h = if ($e.Size -ge 256) { 0 } else { [byte]$e.Size }
        $bw.Write([byte]$w)             # Width   (0 means 256)
        $bw.Write([byte]$h)             # Height  (0 means 256)
        $bw.Write([byte]0)              # Palette colours (0 = no palette)
        $bw.Write([byte]0)              # Reserved
        $bw.Write([uint16]1)            # Colour planes
        $bw.Write([uint16]32)           # Bits per pixel
        $bw.Write([uint32]$e.Bytes.Length)
        $bw.Write([uint32]$offset)
        $offset += $e.Bytes.Length
    }
    foreach ($e in $entries) { $bw.Write($e.Bytes) }

    $bw.Flush()
    [System.IO.File]::WriteAllBytes($DstIco, $out.ToArray())

    foreach ($r in $resized) { $r.Bitmap.Dispose() }
}
finally {
    $source.Dispose()
}

Write-Host "Wrote $DstIco (sizes: $($sizes -join ', '))" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:"             -ForegroundColor Cyan
Write-Host "  1. Re-run CMake configure: cmake -S . -B build"
Write-Host "  2. Rebuild:                cmake --build build"
Write-Host "  3. Verify in Explorer:     right-click build\Magnifier.exe -> Properties"
