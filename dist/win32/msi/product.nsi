; =====================================================
; Euclid NSIS Installer Script
; =====================================================

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WordFunc.nsh"
!include "StrFunc.nsh"

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
Name "Euclid ${VERSION}"
OutFile "${SRCDIR}\euclid-${VERSION}-amd64.exe"
InstallDir "$PROGRAMFILES64\euclid"
InstallDirRegKey HKLM "Software\Euclid" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

; =====================================================
; Version Info
; =====================================================
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName"     "Euclid"
VIAddVersionKey "CompanyName"     "Jens Vogt"
VIAddVersionKey "FileDescription" "Cloud Services"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "LegalCopyright"  "© Jens Vogt"

; =====================================================
; MUI Settings
; =====================================================
!define MUI_ABORTWARNING
!define MUI_ICON   "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

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
; Update check on startup
; =====================================================
Function CheckForUpdates

  inetc::get /SILENT \
    "https://api.github.com/repos/jensvogt/euclid/releases/latest" \
    "$TEMP\euclid_release.json" /END
  Pop $0

  ${If} $0 == "OK"

    ; Parse JSON properly with nsJSON
    nsJSON::Set /file "$TEMP\euclid_release.json"
    Pop $0
    ${If} $0 == "ok"
      nsJSON::Get "tag_name" /end
      Pop $R0  ; $R0 = "${VERSION}" (no leading "v" - euclid tags are bare semver)

      ${VersionCompare} $R0 "${VERSION}" $R2
      ${If} $R2 == 1
        MessageBox MB_YESNO \
          "Version $R0 is available.$\nOpen download page?" \
          IDYES open_browser IDNO done
        open_browser:
          ExecShell "open" "https://github.com/jensvogt/euclid/releases/latest"
          Quit
        done:
      ${EndIf}
    ${EndIf}

  ${EndIf}

  Delete "$TEMP\euclid_release.json"

FunctionEnd

; =====================================================
; Helper: Stop service if running
; =====================================================
Function StopEuclidService

  ; Check if service exists
  SimpleSC::ExistsService "euclid"
  Pop $0
  ${If} $0 == 0

    ; Service exists — check if running
    SimpleSC::GetServiceStatus "euclid"
    Pop $0  ; return code
    Pop $1  ; status code

    ${If} $1 == 4  ; 4 = SERVICE_RUNNING
      DetailPrint "Stopping euclid service..."
      SimpleSC::StopService "euclid" 1 30
      Pop $0
      ${If} $0 != 0
        MessageBox MB_OK "Warning: Could not stop euclid service. Error: $0"
      ${Else}
        DetailPrint "Service stopped successfully."
      ${EndIf}
    ${EndIf}

  ${EndIf}

FunctionEnd

; =====================================================
; Called before installation starts
; =====================================================
Function .onInit
  Call CheckForUpdates
  Call StopEuclidService
FunctionEnd

; =====================================================
; Installer Sections
; =====================================================
Section "Main Application" SecMain

  SetOutPath "$INSTDIR\bin"
  File "${BUILDDIR}\bin\euclid-manager.exe"
  File "${BUILDDIR}\bin\euclid-cli.exe"
  File "${BUILDDIR}\bin\euclid-eam.exe"
  File "${BUILDDIR}\bin\euclid-queues.exe"
  File "${BUILDDIR}\bin\euclid-storage.exe"
  File "${BUILDDIR}\bin\euclid-emo.exe"

  SetOutPath "$INSTDIR\etc"
  File "/oname=euclid.json" "${SRCDIR}\dist\win32\euclid.json"
  ; euclid statically links vcpkg's own libmagic, whose compiled magic database format
  ; isn't guaranteed compatible with any other libmagic's (see Core::ContentTypeUtils) -
  ; take the one this exact build produced instead of a separately maintained copy.
  File "/oname=magic.mgc" "${BUILDDIR}\vcpkg_installed\x64-windows-static\share\libmagic\misc\magic.mgc"
  File "${SRCDIR}\dist\linux\etc\ssh_host_key"
  File "${SRCDIR}\dist\linux\etc\euclid_cert.crt"
  File "${SRCDIR}\dist\linux\etc\euclid_cert.key"

  ; Create empty directories
  CreateDirectory "$INSTDIR\log"
  CreateDirectory "$INSTDIR\init"
  CreateDirectory "$INSTDIR\data\tmp"
  CreateDirectory "$INSTDIR\data\backup"
  CreateDirectory "$INSTDIR\data\run"
  CreateDirectory "$INSTDIR\data\eam"
  CreateDirectory "$INSTDIR\data\eqm"
  CreateDirectory "$INSTDIR\data\queues"
  CreateDirectory "$INSTDIR\data\esm"
  CreateDirectory "$INSTDIR\data\storage"
  CreateDirectory "$INSTDIR\frontend"

  ; Copy frontend recursively, if it was built (see release.yml - skipped when the
  ; euclid-ui repo doesn't exist yet, so the service falls back to no static UI)
  SetOutPath "$INSTDIR\frontend"
  File /nonfatal /r "${SRCDIR}\frontend\euclid-ui\dist\euclid-ui\browser\*.*"

  ; Write registry for uninstaller
  WriteRegStr HKLM "Software\Euclid" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Euclid" \
    "DisplayName" "Euclid"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Euclid" \
    "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Euclid" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Euclid" \
    "Publisher" "Jens Vogt"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Euclid" \
    "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Euclid" \
    "NoRepair" 1

  ; Write uninstaller
  WriteUninstaller "$INSTDIR\uninstall.exe"

SectionEnd

; =====================================================
; Windows Service
; =====================================================
Section "Windows Service" SecService

  ; Stop and remove existing service if present
  SimpleSC::StopService "euclid" 1 30
  SimpleSC::RemoveService "euclid"

  ; Install service (euclid-manager spawns euclid-eam/euclid-queues/euclid-emo as
  ; subprocesses, resolved via PATH, so $INSTDIR\bin is added to the service's PATH)
  SimpleSC::InstallService "euclid" "Euclid" 16 2 \
    `"$INSTDIR\bin\euclid-manager.exe" --config "$INSTDIR\etc\euclid.json"` "" "" ""
  SimpleSC::SetServiceDescription "euclid" "Euclid Cloud Services"

  ; Start service
  SimpleSC::StartService "euclid" "" 30

  Pop $0
  ${If} $0 <> 0
    MessageBox MB_OK "Service could not be started. Error: $0"
  ${EndIf}

SectionEnd

; =====================================================
; Uninstaller
; =====================================================
Section "Uninstall"

  ; Stop and remove service
  SimpleSC::StopService "euclid" 1 30
  SimpleSC::RemoveService "euclid"

  ; Remove files
  Delete "$INSTDIR\bin\euclid-manager.exe"
  Delete "$INSTDIR\bin\euclid-cli.exe"
  Delete "$INSTDIR\bin\euclid-eam.exe"
  Delete "$INSTDIR\bin\euclid-queues.exe"
  Delete "$INSTDIR\bin\euclid-emo.exe"
  Delete "$INSTDIR\bin\euclid-storage.exe"
  Delete "$INSTDIR\etc\euclid.json"
  Delete "$INSTDIR\etc\magic.mgc"
  Delete "$INSTDIR\etc\ssh_host_key"
  Delete "$INSTDIR\etc\euclid_cert.crt"
  Delete "$INSTDIR\etc\euclid_cert.key"
  Delete "$INSTDIR\uninstall.exe"

  RMDir /r "$INSTDIR\frontend"
  RMDir /r "$INSTDIR\init"
  RMDir "$INSTDIR\log"
  RMDir /r "$INSTDIR\data"
  RMDir "$INSTDIR\etc"
  RMDir "$INSTDIR\bin"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Euclid"
  DeleteRegKey HKLM "Software\Euclid"

SectionEnd
