# Mints a self-signed code-signing certificate and writes it out as a password-protected PFX,
# base64-encoded, ready to paste into a GitHub secret.
#
# WHAT THIS IS FOR, AND WHAT IT IS NOT
#
# It is for exercising the signing path: that the workflow finds a certificate, that signtool runs,
# that the timestamp server answers, that the MSI comes out signed and still validates. All of that
# can go wrong, and none of it needs a real certificate to go wrong in.
#
# It is not for release. A self-signed certificate is trusted by nobody: Windows will still warn,
# SmartScreen will still warn, and a machine that has been told to trust this key has been told to
# trust anything signed with a key sitting in a repository secret. Releases need a certificate from
# a public CA, and since June 2023 the CA/Browser Forum requires the private key for one to live on
# certified hardware - a USB token, an HSM, or a cloud signing service. See "Getting a certificate"
# in dist/win32/msi/README.md for the options and what they cost.
#
#   pwsh dist\win32\msi\make-test-certificate.ps1 -Password "whatever you like"
#
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Password,
    [string] $Subject = "CN=Euclid Test Signing (not for release), O=Jens Vogt",
    [string] $Target,
    [int] $ValidForDays = 825
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Target) { $Target = Join-Path $root "euclid-test-signing.pfx" }

# CodeSigningCert is the EKU that makes Authenticode accept it; without it signtool refuses the
# certificate with a message about no certificates meeting the criteria.
$certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -KeyUsage DigitalSignature `
    -KeyExportPolicy Exportable `
    -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -NotAfter (Get-Date).AddDays($ValidForDays)

$secure = ConvertTo-SecureString -String $Password -Force -AsPlainText
Export-PfxCertificate -Cert $certificate -FilePath $Target -Password $secure | Out-Null

# Removed from the store again: the exported file is the copy that matters, and a signing key left
# in a developer's personal store is one that outlives the reason it was made.
Remove-Item ("Cert:\CurrentUser\My\" + $certificate.Thumbprint) -Force

$base64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($Target))
$base64Path = "$Target.base64"
Set-Content -Path $base64Path -Value $base64 -Encoding ascii -NoNewline

""
"Certificate : $Subject"
"Thumbprint  : $($certificate.Thumbprint)"
"Expires     : $($certificate.NotAfter.ToString('yyyy-MM-dd'))"
"PFX         : $Target"
"Base64      : $base64Path"
""
"Put it in the repository's secrets:"
"  gh secret set WINDOWS_CERT_PFX_BASE64 < `"$base64Path`""
"  gh secret set WINDOWS_CERT_PASSWORD   --body `"<the password you just used>`""
""
"Then delete both files. They are the key; the secret is the only copy that should outlive this."
