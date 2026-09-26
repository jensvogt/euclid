# Draws the two bitmaps the MSI wizard needs, from the branding mark.
#
# Windows Installer's dialogs take BMP and nothing else, at two sizes it will not scale: 493x58 for
# the banner that runs across the top of most pages, and 493x312 for the background of the welcome
# and finish pages. Anything else is stretched or cropped by the installer, which is why these are
# generated rather than exported by hand and hoped over.
#
# Generated rather than committed for the same reason license.rtf is: they are derived from
# dist/branding/euclid-512.png, and a committed copy is one that can quietly stop matching the mark
# it was made from.
#
# Run by the release workflow before "wix build". For a local build, run it once yourself:
#
#   pwsh dist\win32\msi\make-ui-images.ps1
#
[CmdletBinding()]
param(
    [string] $Source,
    [string] $BannerTarget,
    [string] $DialogTarget
)

$ErrorActionPreference = "Stop"

# Resolved here rather than as parameter defaults - Windows PowerShell evaluates those before
# $PSScriptRoot is populated.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Source)       { $Source       = Join-Path $root "..\..\branding\euclid-512.png" }
if (-not $BannerTarget) { $BannerTarget = Join-Path $root "banner.bmp" }
if (-not $DialogTarget) { $DialogTarget = Join-Path $root "dialog.bmp" }

if (-not (Test-Path $Source)) { throw "No mark to draw from at $Source" }

Add-Type -AssemblyName System.Drawing

# The sizes Windows Installer's control definitions give these two bitmaps. Not adjustable.
$bannerWidth  = 493
$bannerHeight = 58
$dialogWidth  = 493
$dialogHeight = 312

# Where the welcome and finish pages start writing. Their text is placed by the WixUI library at
# roughly a third of the way across, so the artwork stays left of it - a mark centred on the canvas
# would sit underneath the title.
$bandWidth = 170

$violet = [System.Drawing.Color]::FromArgb(0x7C, 0x4D, 0xFF)   # the mark's own colour
$tint   = [System.Drawing.Color]::FromArgb(0xF4, 0xF0, 0xFF)   # the same hue, far enough back to read as paper
$white  = [System.Drawing.Color]::White

$mark = [System.Drawing.Image]::FromFile((Resolve-Path $Source))

try {
    # ── Banner ────────────────────────────────────────────────────────────────────────────────
    # White, because the dialog around it is: the banner is the top strip of a page whose body is
    # white, and anything else draws a line across the middle of the window. The title is written
    # on the left by the installer, so the mark goes right, and a rule along the bottom edge ties
    # the strip to the page.
    $banner = New-Object System.Drawing.Bitmap($bannerWidth, $bannerHeight, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($banner)
    try {
        $g.Clear($white)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

        $size = 40
        $g.DrawImage($mark, ($bannerWidth - $size - 18), (($bannerHeight - $size) / 2), $size, $size)

        $pen = New-Object System.Drawing.Pen($violet, 2)
        try { $g.DrawLine($pen, 0, ($bannerHeight - 1), $bannerWidth, ($bannerHeight - 1)) } finally { $pen.Dispose() }
    } finally { $g.Dispose() }

    $banner.Save($BannerTarget, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $banner.Dispose()

    # ── Dialog ────────────────────────────────────────────────────────────────────────────────
    # A tinted band down the left with the mark in it, and paper to the right of it for the text
    # the installer writes there. The rule between the two is the same violet as the mark, which is
    # what makes the band read as deliberate rather than as a discoloured edge.
    $dialog = New-Object System.Drawing.Bitmap($dialogWidth, $dialogHeight, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($dialog)
    try {
        $g.Clear($white)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

        $brush = New-Object System.Drawing.SolidBrush($tint)
        try { $g.FillRectangle($brush, 0, 0, $bandWidth, $dialogHeight) } finally { $brush.Dispose() }

        # Above centre rather than on it: the welcome page's own text sits high, and a mark level
        # with the middle of a 312-pixel canvas reads as sitting below everything else.
        $size = 112
        $g.DrawImage($mark, (($bandWidth - $size) / 2), 74, $size, $size)

        $pen = New-Object System.Drawing.Pen($violet, 2)
        try { $g.DrawLine($pen, $bandWidth, 0, $bandWidth, $dialogHeight) } finally { $pen.Dispose() }
    } finally { $g.Dispose() }

    $dialog.Save($DialogTarget, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $dialog.Dispose()

} finally {
    $mark.Dispose()
}

foreach ($file in @($BannerTarget, $DialogTarget)) {
    $image = [System.Drawing.Image]::FromFile((Resolve-Path $file))
    try {
        "{0}: {1}x{2}, {3}, {4:N0} bytes" -f (Split-Path -Leaf $file), $image.Width, $image.Height, $image.PixelFormat, (Get-Item $file).Length
    } finally { $image.Dispose() }
}
