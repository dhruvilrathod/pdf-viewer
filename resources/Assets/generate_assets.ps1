<#
Generates the MSIX package image assets (Assets\*.png) from the repo's
source icon (icon.png, 256x256) using .NET's System.Drawing -- no external
image tools required. Re-run this whenever icon.png changes.

Usage (from repo root or this folder):
    powershell -ExecutionPolicy Bypass -File resources\Assets\generate_assets.ps1
#>

Add-Type -AssemblyName System.Drawing

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$srcPath = Join-Path $repoRoot "icon.png"
if (-not (Test-Path $srcPath)) { throw "Source icon not found: $srcPath" }

$src = [System.Drawing.Image]::FromFile($srcPath)

function New-SquareIcon($size, $outPath) {
	$bmp = New-Object System.Drawing.Bitmap $size, $size
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
	$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
	$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
	$g.Clear([System.Drawing.Color]::Transparent)
	$g.DrawImage($src, 0, 0, $size, $size)
	$g.Dispose()
	$bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
	$bmp.Dispose()
}

function New-PaddedIcon($width, $height, $iconFrac, $outPath) {
	# Centers a square scaled copy of the source icon (iconFrac of the
	# shorter dimension) on a transparent canvas -- used for the wide tile
	# and splash screen, which are non-square.
	$bmp = New-Object System.Drawing.Bitmap $width, $height
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
	$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
	$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
	$g.Clear([System.Drawing.Color]::Transparent)
	$iconSize = [int]([Math]::Min($width, $height) * $iconFrac)
	$x = [int](($width - $iconSize) / 2)
	$y = [int](($height - $iconSize) / 2)
	$g.DrawImage($src, $x, $y, $iconSize, $iconSize)
	$g.Dispose()
	$bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
	$bmp.Dispose()
}

New-SquareIcon 44  (Join-Path $scriptDir "Square44x44Logo.png")
New-SquareIcon 150 (Join-Path $scriptDir "Square150x150Logo.png")
New-SquareIcon 71  (Join-Path $scriptDir "Square71x71Logo.png")
New-SquareIcon 50  (Join-Path $scriptDir "StoreLogo.png")
New-SquareIcon 24  (Join-Path $scriptDir "Square44x44Logo.targetsize-24.png")
New-PaddedIcon 310 150 0.6  (Join-Path $scriptDir "Wide310x150Logo.png")
New-PaddedIcon 620 300 0.4  (Join-Path $scriptDir "SplashScreen.png")

$src.Dispose()
Write-Host "Assets written to $scriptDir"
