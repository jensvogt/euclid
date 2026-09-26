# Authenticode-signs the files it is given, if this build has a certificate to sign them with.
#
# Called from the release workflow twice: once over the binaries before they are packaged, and once
# over the finished MSIs. The binaries matter as much as the installer - the service executable is
# what UAC names when somebody starts it by hand, and an unsigned one is named "Unknown publisher"
# however well signed the package that delivered it was.
#
# NO CERTIFICATE IS NOT AN ERROR. Forks, pull requests from them, and anybody who checks the
# repository out cannot read the repository's secrets, and a release workflow that fails for them is
# a workflow they cannot use. Without a certificate this writes a notice and exits 0, and the build
# produces unsigned packages - which is what it produced before any of this existed.
#
#   pwsh dist\win32\msi\sign.ps1 -Path euclid-1.2.3-amd64.msi
#   pwsh dist\win32\msi\sign.ps1 -Path cmake-build-release\bin\*.exe
#
# Expects, in the environment:
#   WINDOWS_CERT_PFX_BASE64   base64 of a PFX holding the certificate and its key
#   WINDOWS_CERT_PASSWORD     that PFX's password
#
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string[]] $Path,

    # RFC 3161 countersignature. Without one the signature dies with the certificate: Authenticode
    # trusts a signature past its certificate's expiry only if a timestamp authority attests to when
    # it was made. A release signed today should still verify after the certificate is renewed.
    [string] $TimestampUrl = "http://timestamp.digicert.com",

    [int] $Retries = 5
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:WINDOWS_CERT_PFX_BASE64)) {
    Write-Host "::notice title=Unsigned build::No signing certificate available to this build - the packages will be unsigned."
    exit 0
}

$files = @()
foreach ($pattern in $Path) {
    # -Path rather than -LiteralPath, so the caller can pass bin\*.exe and let this expand it.
    $resolved = @(Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue)
    if (-not $resolved) { throw "Nothing to sign at $pattern" }
    $files += $resolved
}

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*\x64\*" } |
    Sort-Object FullName |
    Select-Object -Last 1
if (-not $signtool) { throw "signtool.exe not found - the Windows SDK is not installed on this runner" }

# Native executables get their exit code checked by hand below. Left at Stop, Windows PowerShell
# turns anything signtool writes to stderr into a terminating error - including on the runs that
# succeed - and the script dies partway through a signature instead of reporting on it.
$ErrorActionPreference = "Continue"

# Written outside the workspace so that no later step can sweep it into an artifact, and deleted as
# soon as it has been imported.
$pfx = Join-Path ([IO.Path]::GetTempPath()) "euclid-signing-$PID.pfx"
$thumbprint = $null

try {
    [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:WINDOWS_CERT_PFX_BASE64))

    # Imported into the store and then signed with /sha1, rather than handed to signtool as /f and
    # /p. signtool echoes the command line it was given when it fails, so a password passed that way
    # is a password printed into a public build log on the day the signing breaks. This way the
    # secret never reaches a command line at all.
    $secure = ConvertTo-SecureString -String $env:WINDOWS_CERT_PASSWORD -Force -AsPlainText
    $imported = Import-PfxCertificate -FilePath $pfx -CertStoreLocation "Cert:\CurrentUser\My" -Password $secure
    $thumbprint = $imported.Thumbprint
    Remove-Item $pfx -Force

    Write-Host "Signing with $($imported.Subject), expires $($imported.NotAfter.ToString('yyyy-MM-dd'))"

    foreach ($file in $files) {
        # The timestamp authority is a third party over the public internet, and it is occasionally
        # busy. signtool fails the whole signature when it cannot reach one, so a transient refusal
        # would otherwise fail the release - hence the retries rather than one attempt.
        $signed = $false
        for ($attempt = 1; $attempt -le $Retries; $attempt++) {
            # /fd and /td both SHA256: the file digest and the timestamp digest are separate
            # settings, and leaving /td unset gets a SHA1 timestamp that modern policies reject.
            & $signtool.FullName sign `
                /sha1 $thumbprint `
                /fd SHA256 `
                /tr $TimestampUrl `
                /td SHA256 `
                /d "Euclid" `
                /du "https://github.com/jensvogt/euclid" `
                $file.FullName | Out-Null
            if ($LASTEXITCODE -eq 0) { $signed = $true; break }

            Write-Host "  attempt $attempt of $Retries failed (exit $LASTEXITCODE)"
            if ($attempt -lt $Retries) { Start-Sleep -Seconds (5 * $attempt) }
        }
        if (-not $signed) { throw "Failed to sign $($file.Name) after $Retries attempts" }

        # Verified as a separate operation, because "signtool sign" reporting success only means it
        # produced a signature - not that the chain and the timestamp check out.
        #
        # Read through Get-AuthenticodeSignature rather than "signtool verify", which writes its
        # complaints to stderr and so cannot be called quietly from Windows PowerShell. Valid is the
        # only status that means what it says; everything else is reported and the build continues.
        #
        # A self-signed certificate lands here, and is meant to: its chain does not lead to a root
        # the machine trusts. That is exactly the difference between a certificate that exercises
        # this pipeline and one that signs a release, and it should be visible in the log rather
        # than smoothed over.
        $signature = Get-AuthenticodeSignature $file.FullName
        $timestamped = $null -ne $signature.TimeStamperCertificate
        if ($signature.Status -eq "Valid" -and $timestamped) {
            Write-Host "  signed and verified: $($file.Name)"
        } elseif (-not $timestamped) {
            throw "Signed $($file.Name) but it carries no timestamp - the signature would expire with the certificate"
        } else {
            Write-Host "::warning title=Signature does not verify::$($file.Name) is signed and timestamped, but the signature does not chain to a trusted root ($($signature.Status)). Expected with a self-signed test certificate; a certificate from a public CA would verify."
        }
    }
} finally {
    if (Test-Path $pfx) { Remove-Item $pfx -Force }
    # The key does not stay in the runner's store. Ephemeral runners throw the whole machine away,
    # but this script also runs on developer machines, where it would not.
    if ($thumbprint) { Remove-Item ("Cert:\CurrentUser\My\" + $thumbprint) -Force -ErrorAction SilentlyContinue }
}
