# The Windows packages

Three MSIs, built by the `build-windows-msi` job in `.github/workflows/release.yml`:

| File | Source | What it installs |
| --- | --- | --- |
| `euclid-<version>-amd64.msi` | `euclid.wxs` | The manager, the modules, the web frontend, the CLI, and the Windows service |
| `euclid-cli-<version>-amd64.msi` | `euclid-cli.wxs` | `euclid-cli.exe` alone, on the machine PATH |
| `euclid-wrk-<version>-amd64.msi` | `euclid-wrk.wxs` | `euclid-wrk.exe` and the `euclid-wrk` service, on a worker host |

They have separate `UpgradeCode`s, so none of them upgrades or uninstalls another and all three can
be installed on one machine.

## The worker package

The Windows counterpart of the `euclid-wrk` DEB and RPM, and its own product for the same reason
those are: a worker host is deliberately not a euclid host, and a manager host has no use for the
binary because the manager already runs applications itself. See `docs/worker-nodes.md`.

Two things about it are worth knowing before deploying one.

**It runs the applications placed on it**, through the same `Core::WindowsProcess` the manager
starts its own with: suspended into a job object that ends with the service, so a killed worker
leaves nothing running behind it. An application asked to stop is signalled, then killed after ten
seconds — a JVM, which does not watch the signal, is always the latter. Nothing has been run on a
live installation yet; treat the first Windows node as an experiment.

**The service is installed but not started.** A worker refuses to start without credentials, and
§3.2 means those come from a login an operator performs — there is nothing the package could ship
instead. `Start="install"` would make that refusal an error inside the install transaction and
`Vital="yes"` would roll the whole thing back, so a first install of a correct package would fail.
The service is `Start="auto"` and left stopped, which is exactly what the Debian package does
(`systemctl enable`, not `systemctl start`). To bring a worker up:

```powershell
# as the principal this node acts as
euclid-cli eam login
copy "$env:USERPROFILE\.euclid\credentials" "C:\Program Files\euclid-wrk\etc\credentials"

# then point it at the installation and start it
notepad "C:\Program Files\euclid-wrk\etc\euclid-wrk.json"   # euclid.worker.endpoint
sc start euclid-wrk
```

The copy is the part that is easy to get wrong and is why `euclid-wrk` has a `--credentials` switch
at all: the service runs as Local System, whose `USERPROFILE` is
`C:\Windows\system32\config\systemprofile`, so credentials written by `euclid-cli` at an
administrator's own prompt are in a directory the service never reads. The MSI passes
`--credentials "[INSTALLFOLDER]etc\credentials"` so there is one path to put the file at, and it
moves with a relocated install.

`euclid-wrk.exe` can also register the service itself — `--install`, `--uninstall`, and
`--foreground` to run it as an ordinary console process — for a hand-built tree with no package.
The service it creates is the same one: same name, same start type, same shape of command line.

An upgrade leaves a configured worker stopped until it is started again or the host reboots, where
the server package leaves the manager running. That is the right way round here: the master
re-places work from a node that stops reporting after one lease period, so a worker being down for a
minute is something the design already handles, and an install that fails outright is not.

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

The CLI and worker packages take the same arguments without `FrontendDir`, which neither of them
contains:

```powershell
wix build dist\win32\msi\euclid-wrk.wxs -arch x64 -ext WixToolset.UI.wixext `
  -d Version=1.2.3 `
  -d BuildDir="$PWD\cmake-build-release" `
  -d SrcDir="$PWD" `
  -o euclid-wrk-1.2.3-amd64.msi

wix msi validate -sice ICE61 euclid-wrk-1.2.3-amd64.msi
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
