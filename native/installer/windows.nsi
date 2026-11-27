Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "Sections.nsh"
!ifndef PRODUCT
!define PRODUCT "UAV Studio"
!endif
!ifndef PRODUCT_ID
!define PRODUCT_ID "UAVStudio"
!endif
!ifndef APP_EXE
!define APP_EXE "uav-studio.exe"
!endif
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_ID}"

Name "${PRODUCT}"
OutFile "${SETUP_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT}"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "FileDescription" "UAV Studio Windows installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "UAV Studio contributors"

!define MUI_FINISHPAGE_RUN "$INSTDIR\app\${APP_EXE}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

LangString MainSection ${LANG_ENGLISH} "${PRODUCT} (required)"
LangString MainSection ${LANG_SIMPCHINESE} "${PRODUCT}（必选）"
LangString DesktopSection ${LANG_ENGLISH} "Create desktop shortcut"
LangString DesktopSection ${LANG_SIMPCHINESE} "创建桌面快捷方式"

Function SetInstallerLanguage
    System::Call 'kernel32::GetUserDefaultUILanguage() i.r0'
    IntOp $0 $0 & 0x3ff
    StrCmp $0 4 0 +2
    Return
    StrCpy $LANGUAGE ${LANG_ENGLISH}
FunctionEnd

Section "$(MainSection)" SecMain
    SectionIn RO
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
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "UAV Studio"
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

Section /o "$(DesktopSection)" SecDesktop
    ClearErrors
    CreateShortcut "$DESKTOP\${PRODUCT}.lnk" "$INSTDIR\app\${APP_EXE}"
    IfErrors 0 +3
    SetErrorLevel 1
    Abort
SectionEnd

Section -Shortcuts
    SectionGetFlags ${SecDesktop} $0
    IntOp $0 $0 & ${SF_SELECTED}
    StrCmp $0 0 0 +2
    Delete "$DESKTOP\${PRODUCT}.lnk"
SectionEnd

Function .onInit
    SetShellVarContext current
    IfSilent silent
    Call SetInstallerLanguage
    IfFileExists "$DESKTOP\${PRODUCT}.lnk" 0 done
    !insertmacro SelectSection ${SecDesktop}
    Goto done

silent:
    ${GetParameters} $R0
    ${GetOptions} $R0 "/DESKTOPSHORTCUT=" $R1
    StrCmp $R1 "1" 0 done
    !insertmacro SelectSection ${SecDesktop}
done:
FunctionEnd

Section "Uninstall"
    SetShellVarContext current
    ClearErrors
    RMDir /r "$INSTDIR\app"
    IfErrors UninstallFailed
    Delete "$SMPROGRAMS\${PRODUCT}.lnk"
    Delete "$DESKTOP\${PRODUCT}.lnk"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    Goto UninstallDone

UninstallFailed:
    SetErrorLevel 1
    Abort

UninstallDone:
SectionEnd
