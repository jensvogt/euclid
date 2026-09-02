<#
.SYNOPSIS
    Adds or removes a directory from the machine PATH, for the euclid-cli installer.

.DESCRIPTION
    Called by dist/win32/msi/cli.nsi on install and uninstall.

    This is a script rather than a few lines of NSIS because reading and rewriting the PATH from
    NSIS is dangerous: a stock makensis build truncates strings at 1024 characters, and a machine
    PATH longer than that - ordinary on a build agent - would be silently cut in half by a
    read-modify-write, taking every other tool on the box with it.
    [Environment]::SetEnvironmentVariable has no such limit and broadcasts the change itself, so
    a newly opened terminal picks it up.

    Doing it in a file rather than as an inline "powershell -Command" also keeps the quoting
    honest: NSIS has no backslash escapes, and the same string has to survive NSIS, cmd.exe and
    PowerShell before it means anything.

.PARAMETER Directory
    The directory to add or remove. Compared case-insensitively and without a trailing separator,
    so re-running the installer does not add a second copy.

.PARAMETER Action
    Add or Remove.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Directory,

    [Parameter(Mandatory = $true)]
    [ValidateSet('Add', 'Remove')]
    [string] $Action
)

$ErrorActionPreference = 'Stop'

function Get-PathEntries {
    param([string] $Value)
    if ([string]::IsNullOrEmpty($Value)) { return @() }
    return $Value -split ';' | Where-Object { $_ -ne '' }
}

$wanted = $Directory.TrimEnd('\')
$current = [Environment]::GetEnvironmentVariable('Path', 'Machine')

# Both wrapped in @() so .Count below is always the number of entries. A pipeline that produces
# one item yields a bare string and one that produces none yields $null, and comparing the
# .Count of those two decides here whether PATH gets rewritten at all.
$entries = @(Get-PathEntries $current)

# Every comparison ignores case and a trailing separator, because "C:\Program Files\euclid-cli\bin"
# and "c:\program files\euclid-cli\bin\" are the same directory to Windows and would otherwise
# accumulate as separate PATH entries across repeated installs.
$others = @($entries | Where-Object { $_.TrimEnd('\') -ne $wanted })

switch ($Action) {
    'Add' {
        if ($others.Count -eq $entries.Count) {
            $updated = @($others) + $wanted
        } else {
            # Already there under some spelling - leave PATH exactly as it is rather than
            # rewriting it to say the same thing.
            Write-Output "$wanted is already on the PATH"
            exit 0
        }
    }
    'Remove' {
        if ($others.Count -eq $entries.Count) {
            Write-Output "$wanted is not on the PATH"
            exit 0
        }
        $updated = @($others)
    }
}

[Environment]::SetEnvironmentVariable('Path', ($updated -join ';'), 'Machine')
Write-Output "PATH updated: $Action $wanted"
