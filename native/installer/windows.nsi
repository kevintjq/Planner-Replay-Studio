Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!ifndef PRODUCT
!define PRODUCT "Planner Replay Studio"
!endif
!ifndef PRODUCT_ID
!define PRODUCT_ID "PlannerReplayNative"
!endif
!ifndef APP_EXE
!define APP_EXE "PlannerReplayNative.exe"
!endif
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_ID}"

Name "${PRODUCT}"
OutFile "${SETUP_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "FileDescription" "Planner Replay Studio Windows installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Planner Replay Studio contributors"

!define MUI_FINISHPAGE_RUN "$INSTDIR\app\${APP_EXE}"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Function .onInit
    SetShellVarContext current
    IfSilent +2
    Call SetInstallerLanguage
FunctionEnd

Function SetInstallerLanguage
    System::Call 'kernel32::GetUserDefaultUILanguage() i.r0'
    IntOp $0 $0 & 0x3ff
    StrCmp $0 4 0 +2
    Return
    StrCpy $LANGUAGE ${LANG_ENGLISH}
FunctionEnd

Section
    SetOutPath "$INSTDIR\app"
    SetOverwrite on
    ClearErrors
    File /r "${APP_DIR}\*"
    IfErrors InstallFailed
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    IfErrors InstallFailed
    CreateShortcut "$SMPROGRAMS\${PRODUCT}.lnk" "$INSTDIR\app\${APP_EXE}"
    IfErrors InstallFailed
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Planner Replay Studio"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\app\${APP_EXE}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    Goto InstallDone

InstallFailed:
    SetErrorLevel 1
    Abort

InstallDone:
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    ClearErrors
    RMDir /r "$INSTDIR\app"
    IfErrors UninstallFailed
    Delete "$SMPROGRAMS\${PRODUCT}.lnk"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    Goto UninstallDone

UninstallFailed:
    SetErrorLevel 1
    Abort

UninstallDone:
SectionEnd
