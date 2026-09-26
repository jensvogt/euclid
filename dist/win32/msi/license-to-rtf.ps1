# Turns the repository's LICENSE into the RTF the MSI licence dialog needs.
#
# Windows Installer's licence control reads RTF and nothing else, and euclid's licence is the plain
# text MPL-2.0 that every other package ships. Converting it here rather than committing a second
# copy keeps one source of truth: a licence that is edited in one place cannot end up saying two
# different things depending on which package somebody installed.
#
# Run by the release workflow before "wix build". For a local build, run it once yourself - the
# generated file is deliberately not committed, being derived.
#
#   pwsh dist\win32\msi\license-to-rtf.ps1
#
[CmdletBinding()]
param(
    [string] $Source,
    [string] $Target
)

$ErrorActionPreference = "Stop"

# Resolved here rather than as parameter defaults: Windows PowerShell evaluates those before
# $PSScriptRoot is populated, so the defaults came out empty and the script only worked when both
# paths were passed.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Source) { $Source = Join-Path $root "..\..\..\LICENSE" }
if (-not $Target) { $Target = Join-Path $root "license.rtf" }

if (-not (Test-Path $Source)) { throw "No licence to convert at $Source" }

$text = Get-Content $Source -Raw

# The three characters RTF reads as syntax. Backslash first, or the escapes added for the braces
# would themselves be escaped a moment later.
$text = $text.Replace("\", "\\").Replace("{", "\{").Replace("}", "\}")

# Paragraphs, not line breaks: the MPL is wrapped at around eighty columns for a terminal, and a
# dialog a third of that width would show every one of those wraps as a ragged line ending. A blank
# line is a real paragraph break; a single newline is just where the text file happened to wrap.
$text = $text -replace "\r\n", "`n"
$paragraphs = $text -split "`n`n"
$body = ($paragraphs | ForEach-Object { ($_ -replace "`n", " ").Trim() }) -join "\par\par`r`n"

# Segoe UI at 9pt, which is what every other dialog in the installer uses.
$rtf = "{\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fnil\fcharset0 Segoe UI;}}`r`n\viewkind4\uc1\pard\f0\fs18 " +
       $body + "\par`r`n}`r`n"

Set-Content -Path $Target -Value $rtf -Encoding ascii -NoNewline

"{0} -> {1} ({2:N0} bytes)" -f (Resolve-Path $Source), (Resolve-Path $Target), (Get-Item $Target).Length
