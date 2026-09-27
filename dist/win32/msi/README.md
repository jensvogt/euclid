# The Windows packages

Two MSIs, built by the `build-windows-msi` job in `.github/workflows/release.yml`:

| File | Source | What it installs |
| --- | --- | --- |
| `euclid-<version>-amd64.msi` | `euclid.wxs` | The manager, the modules, the web frontend, the CLI, and the Windows service |
| `euclid-cli-<version>-amd64.msi` | `euclid-cli.wxs` | `euclid-cli.exe` alone, on the machine PATH |

They have separate `UpgradeCode`s, so neither upgrades or uninstalls the other and both can be
installed on one machine.

WiX is pinned to **5.0.2**, including `WixToolset.UI.wixext`. WiX 6 and 7 require accepting the Open
Source Maintenance Fee EULA, and an unpinned `wix extension add` installs a 7.x extension that 5
cannot load — it is accepted with a warning and then fails the build with "extension could not be
found".

## Building one locally

```powershell
dotnet tool install --global wix --version 5.0.2
wix extension add -g WixToolset.UI.wixext/5.0.2

pwsh dist\win32\msi\license-to-rtf.ps1      # writes license.rtf
pwsh dist\win32\msi\make-ui-images.ps1      # writes banner.bmp and dialog.bmp

wix build dist\win32\msi\euclid.wxs -arch x64 -ext WixToolset.UI.wixext `
  -d Version=1.2.3 `
  -d BuildDir="$PWD\cmake-build-release" `
  -d SrcDir="$PWD" `
  -d FrontendDir="$PWD\frontend\euclid-web\dist\euclid-web\browser" `
  -o euclid-1.2.3-amd64.msi

wix msi validate -sice ICE61 euclid-1.2.3-amd64.msi
```

`-arch x64` is not optional: without it every component is packaged as 32-bit while the directories
it installs into are `ProgramFiles64Folder`, which ICE80 reports and which Windows resolves through
the WOW64 view of Program Files. ICE61 is suppressed because `AllowSameVersionUpgrades` always trips
it, and that attribute is deliberate — reinstalling a version over itself is how a build gets tested.

`license.rtf`, `banner.bmp` and `dialog.bmp` are generated and gitignored. They are derived from
`LICENSE` and `dist/branding/euclid-512.png`, and a committed copy is one that can quietly stop
matching what it was made from.

## Signing

`sign.ps1` signs whatever it is given, and is called twice by the workflow: over the executables
before they are packaged, and over each finished MSI. Both matter — the service executable is what
UAC names when somebody starts it by hand, and an unsigned one is "Unknown publisher" however well
signed the package that delivered it was.

It reads two secrets from the environment and **does nothing at all if the first is empty**, so that
forks and pull requests, which cannot read the repository's secrets, still get a working release
build:

| Secret | Contents |
| --- | --- |
| `WINDOWS_CERT_PFX_BASE64` | base64 of a PFX holding the code-signing certificate and its key |
| `WINDOWS_CERT_PASSWORD` | that PFX's password |

The certificate is imported into the user's store and signed with `/sha1 <thumbprint>` rather than
passed as `signtool /f /p`. signtool echoes the command line it was given when it fails, so a
password passed that way is a password printed into a public build log on the day signing breaks.

Everything is timestamped (`/tr`, RFC 3161). Without a countersignature the signature dies with the
certificate; with one it keeps verifying after the certificate is renewed. A signed file that comes
back without a timestamp fails the build.

### Getting a certificate

**For testing the pipeline**, `make-test-certificate.ps1` mints a self-signed one:

```powershell
pwsh dist\win32\msi\make-test-certificate.ps1 -Password "<a password>"
gh secret set WINDOWS_CERT_PFX_BASE64 < dist\win32\msi\euclid-test-signing.pfx.base64
gh secret set WINDOWS_CERT_PASSWORD --body "<that password>"
```

Then delete both files it wrote. This proves the wiring — signtool runs, the timestamp authority
answers, the MSI comes out signed and still installs — and nothing else. A self-signed certificate is
trusted by nobody: Windows and SmartScreen still warn, `signtool verify` still fails, and the
workflow logs a warning saying so on every file. Do not ship with it.

**For releases**, the key has to come from a public CA, and since June 2023 the CA/Browser Forum
requires its private key to live on certified hardware — which rules out a PFX in a repository
secret. The options, cheapest first:

- **Azure Trusted Signing** — around $10/month, Microsoft-operated, no hardware to own or plug in,
  and it has a GitHub Action. Needs a verified organisation (or an individual identity, with a
  three-year history requirement). Signing moves from `signtool /sha1` to the `azure/trusted-signing-action`
  action or `AzureSignTool`; the two `sign.ps1` calls in the workflow are the only places that change.
- **OV certificate on a hardware token or cloud HSM** — DigiCert, Sectigo, SSL.com, roughly
  €300–600/year. Signing from a hosted GitHub runner then means either the CA's cloud signing service
  or a self-hosted runner with the token attached.
- **EV certificate** — same shape, more money, and the reason to pay it is that EV-signed binaries
  start with SmartScreen reputation instead of earning it over some number of downloads.

None of these can be obtained by running a script: they all require buying and an identity check
against the legal entity that will be named as the publisher.
