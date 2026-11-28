param(
    [Parameter(Mandatory = $true)][string]$AppDirectory,
    [Parameter(Mandatory = $true)][string]$SetupFile,
    [string]$Version,
    [Parameter(Mandatory = $true)][string]$QtVersion,
    [Parameter(Mandatory = $true)][string]$OpenCvDirectory,
    [string]$FFmpegDirectory = $env:FFmpeg_ROOT,
    [string]$QtDirectory,
    [string]$QtLicenseDirectory,
    [string]$VcpkgDirectory = $env:VCPKG_INSTALLATION_ROOT,
    [string]$YamlCppLicenseFile,
    [string]$NsisCompiler,
    [string]$DisplayVersion = $Version,
    [string]$NativeRvizHostBinary,
    [switch]$AllowVersionOverride
)

$ErrorActionPreference = 'Stop'
$AppDirectory = (Resolve-Path $AppDirectory).Path
$OpenCvDirectory = (Resolve-Path -LiteralPath $OpenCvDirectory).Path
$SetupFile = [System.IO.Path]::GetFullPath($SetupFile)
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$metadata = Get-Content -LiteralPath (Join-Path $root 'app-metadata.json') -Raw | ConvertFrom-Json
if (!$Version) { $Version = $metadata.version }
if (!$DisplayVersion) { $DisplayVersion = $Version }
if ($Version -cne $metadata.version -and !$AllowVersionOverride) { throw 'Package Version must match app-metadata.json; isolated version fixtures may explicitly use -AllowVersionOverride.' }
if ($metadata.schema_version -ne 1 -or [string]::IsNullOrWhiteSpace($metadata.author) -or $metadata.project_name -notmatch '^[A-Za-z0-9_.-]+$') { throw 'Invalid central application metadata.' }
. "$PSScriptRoot/native-rviz-host.ps1"
. "$PSScriptRoot/windows-package.ps1"
. "$PSScriptRoot/windows-documentation.ps1"
$appExe = Join-Path $AppDirectory 'uav-studio.exe'
$cliExe = Join-Path $AppDirectory 'uav-studio-cli.exe'
if ($Version -notmatch '^\d+\.\d+\.\d+$' -or $QtVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw 'Version and QtVersion must be numeric major.minor.patch versions.'
}
if ($SetupFile.StartsWith($AppDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'SetupFile must be outside AppDirectory to avoid including the installer in itself.'
}
foreach ($file in @($appExe, $cliExe)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing application binary: $file" }
    if (!$AllowVersionOverride) {
        $identity = (Get-Item -LiteralPath $file).VersionInfo
        if ($identity.FileVersion -cne $Version -or $identity.ProductName -cne $metadata.app_name -or $identity.CompanyName -cne $metadata.author) {
            throw "Application PE metadata differs from app-metadata.json: $file. Rebuild both product executables before packaging."
        }
    }
}
if (!$FFmpegDirectory -or !(Test-Path "$FFmpegDirectory/include/libavformat/avformat.h")) {
    throw 'FFmpegDirectory must point to the same development/shared package used by CMake.'
}
$FFmpegDirectory = (Resolve-Path -LiteralPath $FFmpegDirectory).Path
foreach ($file in @('LICENSE', 'README.txt', 'bin/ffmpeg.exe', 'bin/ffprobe.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $FFmpegDirectory $file) -PathType Leaf)) { throw "Missing FFmpeg distribution file: $file" }
}
foreach ($library in @('avformat', 'avcodec', 'avutil', 'swscale', 'swresample')) {
    if (!(Get-ChildItem "$FFmpegDirectory/bin/$library-*.dll" -File)) { throw "Missing shared FFmpeg library: $library" }
}
$stagingInput = Assert-FreshWindowsStaging $AppDirectory $FFmpegDirectory
$deployQt = if ($QtDirectory) { Join-Path $QtDirectory 'bin/windeployqt.exe' } else { (Get-Command windeployqt.exe -ErrorAction Stop).Source }
$deployQt = (Resolve-Path -LiteralPath $deployQt).Path
if (!$NsisCompiler) {
    $makensis = Get-Command makensis.exe -ErrorAction SilentlyContinue
    $NsisCompiler = if ($makensis) { $makensis.Source } else { "${env:ProgramFiles(x86)}\NSIS\makensis.exe" }
}
$NsisCompiler = (Resolve-Path -LiteralPath $NsisCompiler).Path

function FindNotice([string[]]$paths, [string]$name) {
    $found = $paths | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
    if (!$found) { throw "Missing $name license/notice. Checked: $($paths -join ', ')" }
    return (Resolve-Path -LiteralPath $found).Path
}
$notices = [ordered]@{
    'opencv-LICENSE.txt' = FindNotice @("$OpenCvDirectory/opencv-LICENSE.txt", "$OpenCvDirectory/LICENSE") 'OpenCV'
    'opencv-ffmpeg-LICENSE.txt' = FindNotice @("$OpenCvDirectory/opencv-ffmpeg-LICENSE.txt", "$OpenCvDirectory/etc/licenses/ffmpeg-license.txt") 'OpenCV FFmpeg plugin'
}
foreach ($dependency in @('bzip2', 'lz4', 'yaml-cpp')) {
    $candidates = @()
    if ($dependency -eq 'yaml-cpp' -and $YamlCppLicenseFile) { $candidates += $YamlCppLicenseFile }
    if ($VcpkgDirectory) { $candidates += "$VcpkgDirectory/installed/x64-windows-static-md/share/$dependency/copyright" }
    $notices["$dependency.txt"] = FindNotice $candidates $dependency
}

$opencvDlls = Get-ChildItem $OpenCvDirectory -Recurse -File -Filter 'opencv*.dll'
if (!$opencvDlls -or !($opencvDlls | Where-Object Name -Like 'opencv_videoio_ffmpeg*_64.dll')) {
    throw 'OpenCV and its video decoding plugin are required.'
}
$opencvDlls | Copy-Item -Destination $AppDirectory

& $deployQt --release --no-compiler-runtime --qmldir "$root/native/qml" $appExe $cliExe
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }
# The unattended runtime check uses Qt's official offscreen platform. windeployqt
# detects qwindows for the desktop app but does not include qoffscreen itself.
$qtRoot = Split-Path -Parent (Split-Path -Parent $deployQt)
$offscreenPlugin = Join-Path $qtRoot 'plugins/platforms/qoffscreen.dll'
if (!(Test-Path -LiteralPath $offscreenPlugin -PathType Leaf)) {
    throw 'The selected Qt distribution must include its official qoffscreen.dll platform plugin.'
}
Copy-Item -LiteralPath $offscreenPlugin -Destination (Join-Path $AppDirectory 'platforms/qoffscreen.dll')
# windeployqt may deploy its bundled FFmpeg. Restore the package linked by our decoder.
Copy-Item "$FFmpegDirectory/bin/*.dll", "$FFmpegDirectory/bin/ffmpeg.exe", "$FFmpegDirectory/bin/ffprobe.exe" $AppDirectory
if (!(Test-Path (Join-Path $AppDirectory 'imageformats/qtiff.dll'))) {
    throw 'Qt Image Formats (qtimageformats / qtiff.dll) is required for TIFF research exports.'
}

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

$documentation = Copy-WindowsDocumentation $AppDirectory $root
$documentation | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $AppDirectory 'documentation.json')
$licenses = New-Item -ItemType Directory -Force -Path "$AppDirectory/licenses"
Copy-Item "$FFmpegDirectory/LICENSE" "$licenses/FFmpeg.txt"
Copy-Item "$FFmpegDirectory/README.txt" "$licenses/FFmpeg-build-README.txt"
foreach ($notice in $notices.GetEnumerator()) {
    Copy-Item -LiteralPath $notice.Value -Destination (Join-Path $licenses $notice.Key)
}
if (Test-Path -LiteralPath "$OpenCvDirectory/etc/licenses") {
    New-Item -ItemType Directory -Force -Path "$licenses/OpenCV-third-party" | Out-Null
    Copy-Item "$OpenCvDirectory/etc/licenses/*" -Destination "$licenses/OpenCV-third-party" -Recurse -Force
}
foreach ($license in @('LGPL-3.0-only', 'GPL-3.0-only')) {
    $destination = Join-Path $licenses "Qt-$license.txt"
    if ($QtLicenseDirectory) {
        Copy-Item -LiteralPath (Join-Path $QtLicenseDirectory "$license.txt") -Destination $destination
    } else {
        Invoke-WebRequest "https://raw.githubusercontent.com/qt/qtbase/v$QtVersion/LICENSES/$license.txt" -OutFile $destination
    }
    if ((Get-Item -LiteralPath $destination).Length -lt 1000 -or (Get-Content -LiteralPath $destination -Raw) -notmatch 'GNU') {
        throw "Invalid Qt license document: $license"
    }
}

& "$FFmpegDirectory/bin/ffmpeg.exe" -version 2>&1 | Set-Content -Encoding utf8 "$licenses/FFmpeg-version.txt"
if ($LASTEXITCODE -ne 0) { throw 'Could not record the shipped FFmpeg build version.' }
& "$FFmpegDirectory/bin/ffmpeg.exe" -buildconf 2>&1 | Set-Content -Encoding utf8 "$licenses/FFmpeg-buildconf.txt"
if ($LASTEXITCODE -ne 0) { throw 'Could not record the shipped FFmpeg build configuration.' }
$nativeBackend = Copy-NativeRvizHost $AppDirectory $root $NativeRvizHostBinary
$nativeBackend | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $AppDirectory 'native-rviz-backend.json')
$runtimePayload = Assert-WindowsRuntimePayload $AppDirectory ([bool]$NativeRvizHostBinary)
$runtimePayload.staging_input = $stagingInput
$runtimePayload | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $AppDirectory 'windows-runtime.json')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $SetupFile) | Out-Null
$includeNativeHost = if ($NativeRvizHostBinary) { 1 } else { 0 }
Copy-Item -LiteralPath (Join-Path $root 'app-metadata.json') -Destination (Join-Path $AppDirectory 'app-metadata.json')
$installManifest = Write-WindowsInstallManifest $AppDirectory $metadata.project_name $Version $metadata.app_name ($metadata.executable_name + '.exe')
$compilerArguments = @('/WX', "/DAPP_DIR=$AppDirectory", "/DSETUP_FILE=$SetupFile", "/DVERSION=$Version", "/DDISPLAY_VERSION=$DisplayVersion",
    "/DPRODUCT=$($metadata.app_name)", "/DPRODUCT_ID=$($metadata.project_name)", "/DAPP_EXE=$($metadata.executable_name).exe",
    "/DAUTHOR=$($metadata.author)", "/DPROJECT_URL=$($metadata.github_url)", "/DCONTACT_EMAIL=$($metadata.email)", "/DINSTALL_MANIFEST_SHA256=$($installManifest.sha256)",
    "/DINCLUDE_NATIVE_RVIZ_HOST=$includeNativeHost", "$PSScriptRoot/windows.nsi")
$quotedCompilerArguments = $compilerArguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
# Explicit process redirection also supports NSIS launchers marked as GUI
# programs, which PowerShell's native pipeline may fail to redirect correctly.
$compilerProcess = Start-Process -FilePath $NsisCompiler -ArgumentList $quotedCompilerArguments -WindowStyle Hidden -PassThru -Wait `
    -RedirectStandardOutput ($SetupFile + '.compile.log') -RedirectStandardError ($SetupFile + '.compile.err.log')
if ($compilerProcess.ExitCode -ne 0 -or !(Test-Path $SetupFile)) { throw "NSIS did not produce the setup executable. See $SetupFile.compile.log and $SetupFile.compile.err.log." }
