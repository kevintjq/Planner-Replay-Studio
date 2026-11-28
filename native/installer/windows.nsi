Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "Sections.nsh"
!include "LogicLib.nsh"
!ifndef PRODUCT
!error "Pass PRODUCT from app-metadata.json (or an isolated fixture)."
!endif
!ifndef PRODUCT_ID
!error "Pass PRODUCT_ID from app-metadata.json (or an isolated fixture)."
!endif
!ifndef APP_EXE
!error "Pass APP_EXE from app-metadata.json (or an isolated fixture)."
!endif
!ifndef DISPLAY_VERSION
!define DISPLAY_VERSION "${VERSION}"
!endif
!ifndef AUTHOR
!error "Pass AUTHOR from app-metadata.json."
!endif
!ifndef PROJECT_URL
!error "Pass PROJECT_URL from app-metadata.json."
!endif
!ifndef CONTACT_EMAIL
!error "Pass CONTACT_EMAIL from app-metadata.json."
!endif
!ifndef INSTALL_MANIFEST_SHA256
!error "Generate uav-studio-install.json and pass INSTALL_MANIFEST_SHA256 before compiling."
!endif
; Standalone is the default even when NSIS is invoked without package.ps1.
; The Linux host is explicit opt-in; installation never probes or starts WSL/ROS.
!ifndef INCLUDE_NATIVE_RVIZ_HOST
!define INCLUDE_NATIVE_RVIZ_HOST 0
!endif
!ifndef ASSOCIATION_ROOT
!define ASSOCIATION_ROOT "Software\Classes"
!endif
!ifndef STARTMENU_DIRECTORY
!define STARTMENU_DIRECTORY "$SMPROGRAMS"
!endif
!ifndef DESKTOP_DIRECTORY
!define DESKTOP_DIRECTORY "$DESKTOP"
!endif
!define PROJECT_PROGID "${PRODUCT_ID}.Project"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_ID}"

Name "${PRODUCT}"
OutFile "${SETUP_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT}"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "FileDescription" "UAV Studio Windows installer"
VIAddVersionKey "FileVersion" "${DISPLAY_VERSION}"
VIAddVersionKey "LegalCopyright" "${AUTHOR}"
VIAddVersionKey "CompanyName" "${AUTHOR}"

Var UpgradeStaging
Var UpgradeState

!define MUI_ICON "${__FILEDIR__}\..\..\assets\icons\uav-studio.ico"
!define MUI_UNICON "${__FILEDIR__}\..\..\assets\icons\uav-studio.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\app\${APP_EXE}"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE EnsureSafeApplicationFolder
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
LangString ExistingInstallFolder ${LANG_ENGLISH} "Installation stopped. Existing files were preserved. Close UAV Studio if it is running, or choose an empty folder when ownership cannot be verified. Details:"
LangString ExistingInstallFolder ${LANG_SIMPCHINESE} "安装已停止，原有文件已保留。若软件正在运行请先关闭；无法验证为本产品时请选择空文件夹。详情："

Function RunUpgradeHelper
    nsExec::ExecToStack '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\windows-upgrade.ps1" -Operation "$R9" -InstallRoot "$INSTDIR" -InstallId "${PRODUCT_ID}" -AppName "${PRODUCT}" -ExecutableName "${APP_EXE}" -UninstallKey "${UNINSTALL_KEY}" -AssociationRoot "${ASSOCIATION_ROOT}" -ManifestHash "${INSTALL_MANIFEST_SHA256}" -PayloadManifestFile "$PLUGINSDIR\payload-manifest.json" -StateFile "$UpgradeState"'
    Pop $0
    Pop $1
    DetailPrint "$1"
!ifdef DIAGNOSTIC_LOG
    FileOpen $2 "${DIAGNOSTIC_LOG}" a
    FileSeek $2 0 END
    FileWrite $2 "$R9: $0 $1$\r$\n"
    FileClose $2
!endif
    StrCmp $0 "0" HelperDone
    StrCmp $0 "error" 0 +2
    StrCpy $0 5
    SetErrorLevel $0
    MessageBox MB_OK|MB_ICONSTOP "$(ExistingInstallFolder)$\r$\n$1" /SD IDOK
    Abort
HelperDone:
    SetErrorLevel 0
FunctionEnd

Function EnsureSafeApplicationFolder
    ; Verification never changes the old payload or registration. A trusted
    ; same-product installation may be upgraded; arbitrary occupied roots may not.
    StrCpy $R9 "Check"
    Call RunUpgradeHelper
FunctionEnd

!macro RegisterProjectExtension EXTENSION
    ReadRegStr $0 HKCU "${ASSOCIATION_ROOT}\${EXTENSION}" ""
    ClearErrors
    ; Keep another application's default; Open With remains available.
    StrCmp $0 "" 0 +2
    WriteRegStr HKCU "${ASSOCIATION_ROOT}\${EXTENSION}" "" "${PROJECT_PROGID}"
    WriteRegStr HKCU "${ASSOCIATION_ROOT}\${EXTENSION}\OpenWithProgids" "${PROJECT_PROGID}" ""
    IfErrors InstallFailed
!macroend

!macro DeleteEmptyAssociationKey KEY
    ; NSIS /ifempty ignores registry values, so explicitly check both kinds.
    ClearErrors
    EnumRegKey $0 HKCU "${ASSOCIATION_ROOT}\${KEY}" 0
    ${If} ${Errors}
        ClearErrors
        EnumRegValue $0 HKCU "${ASSOCIATION_ROOT}\${KEY}" 0
        ${If} ${Errors}
            DeleteRegKey HKCU "${ASSOCIATION_ROOT}\${KEY}"
        ${EndIf}
    ${EndIf}
!macroend

!macro UnregisterProjectExtension EXTENSION
    ReadRegStr $0 HKCU "${ASSOCIATION_ROOT}\${EXTENSION}" ""
    StrCmp $0 "${PROJECT_PROGID}" 0 +2
    DeleteRegValue HKCU "${ASSOCIATION_ROOT}\${EXTENSION}" ""
    DeleteRegValue HKCU "${ASSOCIATION_ROOT}\${EXTENSION}\OpenWithProgids" "${PROJECT_PROGID}"
    !insertmacro DeleteEmptyAssociationKey "${EXTENSION}\OpenWithProgids"
    !insertmacro DeleteEmptyAssociationKey "${EXTENSION}"
!macroend

Function SetInstallerLanguage
    System::Call 'kernel32::GetUserDefaultUILanguage() i.r0'
    IntOp $0 $0 & 0x3ff
    StrCmp $0 4 0 +2
    Return
    StrCpy $LANGUAGE ${LANG_ENGLISH}
FunctionEnd

Section "$(MainSection)" SecMain
    SectionIn RO
    StrCpy $R9 "Prepare"
    Call RunUpgradeHelper
    ReadINIStr $UpgradeStaging "$UpgradeState.ini" "Install" "Staging"
    StrCmp $UpgradeStaging "" InstallFailed
    SetOutPath "$UpgradeStaging\app"
    SetOverwrite on
    ClearErrors
!if ${INCLUDE_NATIVE_RVIZ_HOST} == 1
    File /r /x "uav-studio-install.json" "${APP_DIR}\*"
!else
    File /r /x "rviz_host" /x "uav-studio-install.json" "${APP_DIR}\*"
!endif
!ifdef INSTALL_MANIFEST_FILE
    File /oname=uav-studio-install.json "${INSTALL_MANIFEST_FILE}"
!else
    File /oname=uav-studio-install.json "${APP_DIR}\uav-studio-install.json"
!endif
    IfErrors InstallFailed
    WriteUninstaller "$UpgradeStaging\Uninstall.exe"
    IfErrors InstallFailed
    ; SetOutPath also changes the installer's current directory. Release the
    ; staging/app directory before the helper atomically renames it into place.
    SetOutPath "$PLUGINSDIR"
    StrCpy $R9 "Commit"
    Call RunUpgradeHelper
    SetOutPath "$INSTDIR\app"
    CreateDirectory "${STARTMENU_DIRECTORY}"
    CreateShortcut "${STARTMENU_DIRECTORY}\${PRODUCT}.lnk" "$INSTDIR\app\${APP_EXE}" "" "$INSTDIR\app\${APP_EXE}" 0 SW_SHOWNORMAL "" "UAV Studio — All-in-One Robotics Research Studio"
    IfErrors InstallFailed
    ClearErrors
    WriteRegStr HKCU "${ASSOCIATION_ROOT}\${PROJECT_PROGID}" "" "UAV Studio Project"
    WriteRegStr HKCU "${ASSOCIATION_ROOT}\${PROJECT_PROGID}\DefaultIcon" "" '$\"$INSTDIR\app\${APP_EXE}$\",0'
    WriteRegStr HKCU "${ASSOCIATION_ROOT}\${PROJECT_PROGID}\shell\open\command" "" '$\"$INSTDIR\app\${APP_EXE}$\" $\"%1$\"'
    WriteRegStr HKCU "${ASSOCIATION_ROOT}\Applications\${APP_EXE}" "FriendlyAppName" "UAV Studio"
    WriteRegStr HKCU "${ASSOCIATION_ROOT}\Applications\${APP_EXE}\shell\open\command" "" '$\"$INSTDIR\app\${APP_EXE}$\" $\"%1$\"'
    WriteRegStr HKCU "${ASSOCIATION_ROOT}\Applications\${APP_EXE}\SupportedTypes" ".uavstudio" ""
    WriteRegStr HKCU "${ASSOCIATION_ROOT}\Applications\${APP_EXE}\SupportedTypes" ".slamfpv" ""
    IfErrors InstallFailed
    !insertmacro RegisterProjectExtension ".uavstudio"
    !insertmacro RegisterProjectExtension ".slamfpv"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${DISPLAY_VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${AUTHOR}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "${PROJECT_URL}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Contact" "${CONTACT_EMAIL}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallId" "${PRODUCT_ID}"
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
    CreateDirectory "${DESKTOP_DIRECTORY}"
    CreateShortcut "${DESKTOP_DIRECTORY}\${PRODUCT}.lnk" "$INSTDIR\app\${APP_EXE}" "" "$INSTDIR\app\${APP_EXE}" 0 SW_SHOWNORMAL "" "UAV Studio — All-in-One Robotics Research Studio"
    IfErrors 0 +3
    SetErrorLevel 1
    Abort
SectionEnd

Section -Shortcuts
    SectionGetFlags ${SecDesktop} $0
    IntOp $0 $0 & ${SF_SELECTED}
    StrCmp $0 0 0 +2
    Delete "${DESKTOP_DIRECTORY}\${PRODUCT}.lnk"
SectionEnd

Function .onInit
    SetShellVarContext current
    SetRegView 64
    InitPluginsDir
    File /oname=$PLUGINSDIR\windows-upgrade.ps1 "${__FILEDIR__}\windows-upgrade.ps1"
!ifdef INSTALL_MANIFEST_FILE
    File /oname=$PLUGINSDIR\payload-manifest.json "${INSTALL_MANIFEST_FILE}"
!else
    File /oname=$PLUGINSDIR\payload-manifest.json "${APP_DIR}\uav-studio-install.json"
!endif
    StrCpy $UpgradeState "$PLUGINSDIR\install-transaction.json"
    IfSilent silent
    Call SetInstallerLanguage
    IfFileExists "${DESKTOP_DIRECTORY}\${PRODUCT}.lnk" 0 done
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
    SetRegView 64
    ClearErrors
    RMDir /r "$INSTDIR\app"
    IfErrors UninstallFailed
    ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
    StrCmp $0 "$INSTDIR" 0 ShortcutsDone
    Delete "${STARTMENU_DIRECTORY}\${PRODUCT}.lnk"
    Delete "${DESKTOP_DIRECTORY}\${PRODUCT}.lnk"
ShortcutsDone:
    ; An older uninstaller must not remove a newer install's registrations.
    ReadRegStr $0 HKCU "${ASSOCIATION_ROOT}\${PROJECT_PROGID}\shell\open\command" ""
    StrCmp $0 '$\"$INSTDIR\app\${APP_EXE}$\" $\"%1$\"' 0 AssociationsDone
    !insertmacro UnregisterProjectExtension ".uavstudio"
    !insertmacro UnregisterProjectExtension ".slamfpv"
    DeleteRegKey HKCU "${ASSOCIATION_ROOT}\${PROJECT_PROGID}"
    ReadRegStr $0 HKCU "${ASSOCIATION_ROOT}\Applications\${APP_EXE}\shell\open\command" ""
    StrCmp $0 '$\"$INSTDIR\app\${APP_EXE}$\" $\"%1$\"' 0 AssociationsDone
    DeleteRegKey HKCU "${ASSOCIATION_ROOT}\Applications\${APP_EXE}"
AssociationsDone:
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
    StrCmp $0 "$INSTDIR" 0 UninstallRegistrationDone
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
UninstallRegistrationDone:
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    Goto UninstallDone

UninstallFailed:
    SetErrorLevel 1
    Abort

UninstallDone:
SectionEnd
