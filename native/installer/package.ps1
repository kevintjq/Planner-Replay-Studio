param(
    [Parameter(Mandatory = $true)][string]$AppDirectory,
    [Parameter(Mandatory = $true)][string]$SetupFile,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$QtVersion
)

$ErrorActionPreference = 'Stop'
$AppDirectory = (Resolve-Path $AppDirectory).Path
$SetupFile = [System.IO.Path]::GetFullPath($SetupFile)
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$appExe = Join-Path $AppDirectory 'PlannerReplayNative.exe'

windeployqt --release --no-compiler-runtime --qmldir "$root/native/qml" $appExe
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }

# Use Visual Studio's designated x64 redistributable CRT files for app-local deployment.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Could not find Visual Studio with x64 C++ build tools.' }
$redist = Join-Path $installation 'VC/Redist/MSVC'
$crt = Get-ChildItem $redist -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64/Microsoft.VC143.CRT' } |
    Where-Object { Test-Path (Join-Path $_ 'vcruntime140.dll') } |
    Select-Object -First 1
if (!$crt) { throw 'Could not find the VC143 x64 redistributable CRT files.' }
Copy-Item "$crt/*.dll" $AppDirectory

Copy-Item "$root/native/README.md", "$root/LICENSE", "$PSScriptRoot/THIRD_PARTY.md" $AppDirectory
$licenses = New-Item -ItemType Directory -Force -Path "$AppDirectory/licenses"
foreach ($dependency in @('bzip2', 'lz4')) {
    Copy-Item "$env:VCPKG_INSTALLATION_ROOT/installed/x64-windows-static-md/share/$dependency/copyright" "$licenses/$dependency.txt"
}
foreach ($license in @('LGPL-3.0-only', 'GPL-3.0-only')) {
    Invoke-WebRequest "https://raw.githubusercontent.com/qt/qtbase/v$QtVersion/LICENSES/$license.txt" -OutFile "$licenses/Qt-$license.txt"
}

$makensis = Get-Command makensis.exe -ErrorAction SilentlyContinue
$nsis = if ($makensis) { $makensis.Source } else { "${env:ProgramFiles(x86)}\NSIS\makensis.exe" }
if (!(Test-Path $nsis)) { throw 'NSIS was not found.' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $SetupFile) | Out-Null
& $nsis /WX "/DAPP_DIR=$AppDirectory" "/DSETUP_FILE=$SetupFile" "/DVERSION=$Version" "$PSScriptRoot/windows.nsi"
if ($LASTEXITCODE -ne 0 -or !(Test-Path $SetupFile)) { throw 'NSIS did not produce the setup executable.' }
