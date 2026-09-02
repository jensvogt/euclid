; =====================================================
; Euclid CLI NSIS Installer Script
; =====================================================
;
; The command on its own, for a machine that talks to a euclid running somewhere else. The
; installer in product.nsi already contains euclid-cli.exe, because a server is usually
; administered from the machine it runs on - this is for every machine that is not one, and so
; installs no modules, no service and no certificates.
;
; Deliberately plugin-free, unlike product.nsi: there is no service to stop (SimpleSC) and
; nothing to nag about (INetC/nsJSON), and a CLI installer that cannot fail on a missing plugin
; is one less thing between somebody and a working command.
; =====================================================

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

; =====================================================
; Environment variables
; =====================================================
!ifndef SRCDIR
  !define SRCDIR ".\"
!endif
!ifndef BUILDDIR
  !define BUILDDIR ".\cmake-build-release"
!endif
!ifndef VERSION
  !define VERSION "1.0.0"
!endif

; =====================================================
; General Settings
; =====================================================
Name "Euclid CLI ${VERSION}"
OutFile "${SRCDIR}\euclid-cli-${VERSION}-amd64.exe"
InstallDir "$PROGRAMFILES64\euclid-cli"
InstallDirRegKey HKLM "Software\EuclidCLI" "InstallDir"
; Admin because this installs under Program Files and edits the machine PATH. A per-user install
; would avoid that, but then the command is missing for everybody else on a shared build agent,
; which is exactly where this package gets used.
RequestExecutionLevel admin
SetCompressor /SOLID lzma

; =====================================================
; Version Info
; =====================================================
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName"     "Euclid CLI"
VIAddVersionKey "CompanyName"     "Jens Vogt"
VIAddVersionKey "FileDescription" "Euclid command line interface"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "LegalCopyright"  "© Jens Vogt"

; =====================================================
; MUI Settings
; =====================================================
!define MUI_ABORTWARNING
!define MUI_ICON   "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_FINISHPAGE_TEXT "euclid-cli is installed and on the PATH.$\r$\n$\r$\nOpen a new terminal and run:$\r$\n$\r$\n    euclid-cli --endpoint https://your-euclid:5566 eam login --user <name> --password <secret>$\r$\n$\r$\nAn already-open terminal will not see the new PATH."

; =====================================================
; Pages
; =====================================================
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; =====================================================
; Installer Sections
; =====================================================
Section "Euclid CLI" SecCli

  SetOutPath "$INSTDIR\bin"
  File "${BUILDDIR}\bin\euclid-cli.exe"

  ; The PATH edit is a shipped PowerShell script rather than a few lines of NSIS - see the
  ; comments in euclid-path.ps1 for why touching PATH from NSIS is a good way to destroy it.
  ; Installed next to the uninstaller because the uninstaller needs it too.
  SetOutPath "$INSTDIR"
  File "${SRCDIR}\dist\win32\msi\euclid-path.ps1"

  DetailPrint "Adding $INSTDIR\bin to the system PATH..."
  nsExec::ExecToLog 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\euclid-path.ps1" -Directory "$INSTDIR\bin" -Action Add'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_OK "Could not add $INSTDIR\bin to the PATH (error $0).$\r$\nAdd it by hand, or run euclid-cli by its full path."
  ${EndIf}

  ; Write registry for uninstaller. A separate key from the server installer's "Euclid", so the
  ; two appear as what they are - two products - and uninstalling one leaves the other alone.
  WriteRegStr HKLM "Software\EuclidCLI" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\EuclidCLI" \
    "DisplayName" "Euclid CLI"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\EuclidCLI" \
    "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\EuclidCLI" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\EuclidCLI" \
    "Publisher" "Jens Vogt"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\EuclidCLI" \
    "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\EuclidCLI" \
    "NoRepair" 1

  WriteUninstaller "$INSTDIR\uninstall.exe"

SectionEnd

; =====================================================
; Uninstaller
; =====================================================
Section "Uninstall"

  DetailPrint "Removing $INSTDIR\bin from the system PATH..."
  nsExec::ExecToLog 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\euclid-path.ps1" -Directory "$INSTDIR\bin" -Action Remove'
  Pop $0

  Delete "$INSTDIR\bin\euclid-cli.exe"
  ; After the PATH edit above, which needs it.
  Delete "$INSTDIR\euclid-path.ps1"
  Delete "$INSTDIR\uninstall.exe"

  RMDir "$INSTDIR\bin"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\EuclidCLI"
  DeleteRegKey HKLM "Software\EuclidCLI"

SectionEnd
