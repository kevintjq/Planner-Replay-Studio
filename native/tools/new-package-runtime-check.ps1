param(
    [Parameter(Mandatory = $true)][string]$AppDirectory,
    [Parameter(Mandatory = $true)][string]$NsisCompiler,
    [string]$Version = '0.1.1',
    [string]$DisplayVersion = $Version,
    [string]$VideoFile,
    [string]$ArtifactDirectory,
    [ValidateSet('Standalone', 'OptionalNativeRviz')][string]$RuntimeProfile = 'Standalone',
    [switch]$Offscreen
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
. "$repository/native/installer/native-rviz-host.ps1"
. "$repository/native/installer/windows-package.ps1"
$metadata = Get-Content -LiteralPath (Join-Path $repository 'app-metadata.json') -Raw | ConvertFrom-Json
$AppDirectory = (Resolve-Path -LiteralPath $AppDirectory).Path
$NsisCompiler = (Resolve-Path -LiteralPath $NsisCompiler).Path
if ($VideoFile) { $VideoFile = (Resolve-Path -LiteralPath $VideoFile).Path }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Expected a numeric major.minor.patch version.' }
$runId = [Guid]::NewGuid().ToString('N').Substring(0, 12)
if (!$ArtifactDirectory) { $ArtifactDirectory = Join-Path $repository "captures/package-runtime-$runId" }
$ArtifactDirectory = [IO.Path]::GetFullPath($ArtifactDirectory)
$allowedRoot = [IO.Path]::GetFullPath($repository) + [IO.Path]::DirectorySeparatorChar
if (!$ArtifactDirectory.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Runtime acceptance artifacts must remain inside this repository.'
}
if (Test-Path -LiteralPath $ArtifactDirectory) { throw 'Choose a fresh artifact directory.' }
New-Item -ItemType Directory -Path $ArtifactDirectory | Out-Null

$product = "UAV Studio Runtime Acceptance $runId"
$productId = "UAVStudioRuntimeAcceptance$runId"
$associationRoot = "Software\UAVStudioRuntimeAcceptance\$runId\Classes"
$install = Join-Path $ArtifactDirectory 'i'
$installedApp = Join-Path $install 'app'
$start = Join-Path $ArtifactDirectory 'shortcuts/start'
$desktop = Join-Path $ArtifactDirectory 'shortcuts/desktop'
$setup = Join-Path $ArtifactDirectory 'runtime-acceptance-setup.exe'
$uninstallRoot = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$productId"
$report = [ordered]@{
    kind = 'complete-package-runtime'; production_registry_touched = $false
    manual_gui_acceptance = $false; source_video_checked = [bool]$VideoFile
    offscreen_runtime = [bool]$Offscreen
    runnable_distribution_verified = $false
    offscreen_partial_runtime_verified = $false
    opengl_output_verified = $false
    package_profile = $RuntimeProfile
    dependency_path = 'Installed application directory and Windows system only'
    environment_isolation = 'Minimal PATH and cleared Qt/ROS/Python environment on the current developer OS'
    clean_windows_without_wsl_verified = $false
    clean_environment_limit = 'WSL/ROS/Python have not been removed from the host OS. This run does not certify a clean Windows VM.'
    native_backend_process_activity_verified = $false
    launched_processes = @()
    checks = @(); skipped_checks = @(); success = $false
}

function Check([bool]$condition, [string]$name) {
    $script:report.checks += [ordered]@{name = $name; passed = $condition}
    if (!$condition) { throw "Runtime check failed: $name" }
}
function RunProgram([string]$program, [string[]]$arguments, [string]$name, [int]$timeoutSeconds = 40, [switch]$NativeWindow) {
    # Native-window acceptance needs an exposed Qt surface: SW_HIDE prevents the
    # Windows software scene graph's asynchronous grab from completing. Keep
    # console/helper tools hidden; offscreen checks still do not certify OpenGL.
    $quoted = $arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
    $windowStyle = if ($NativeWindow) { 'Normal' } else { 'Hidden' }
    $process = Start-Process -FilePath $program -ArgumentList $quoted -WorkingDirectory $ArtifactDirectory -WindowStyle $windowStyle `
        -RedirectStandardOutput (Join-Path $ArtifactDirectory "$name.out.log") `
        -RedirectStandardError (Join-Path $ArtifactDirectory "$name.err.log") -PassThru
    $launch=[ordered]@{name=$name;pid=$process.Id;program=$program;arguments=$arguments;window_style=$windowStyle}
    $script:report.launched_processes += $launch
    if (!$process.WaitForExit($timeoutSeconds * 1000)) {
        $process.Kill($true)
        $process.WaitForExit()
        throw "$name exceeded $timeoutSeconds seconds."
    }
    $process.WaitForExit()
    $launch.exit_code=$process.ExitCode
    Check ($process.ExitCode -eq 0) "$name exits successfully (actual $($process.ExitCode))"
}

$savedEnvironment = @{}
$environmentNames = @('PATH', 'QT_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'QT_QPA_PLATFORM', 'QT_QPA_FONTDIR', 'QT_QUICK_BACKEND', 'QT_FORCE_STDERR_LOGGING')
$environmentNames += @([Environment]::GetEnvironmentVariables('Process').Keys | Where-Object {
    $_ -match '^(QT|QSG|QML|UAV_STUDIO|ROS|AMENT|COLCON|CATKIN|CONDA)_' -or $_ -match '^PYTHON' -or $_ -in @('VIRTUAL_ENV', 'CMAKE_PREFIX_PATH', 'LD_LIBRARY_PATH')
})
foreach ($name in @($environmentNames | Select-Object -Unique)) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $payload = Assert-WindowsRuntimePayload $AppDirectory ($RuntimeProfile -eq 'OptionalNativeRviz')
    $backendManifest = Get-Content -LiteralPath (Join-Path $AppDirectory 'native-rviz-backend.json') -Raw | ConvertFrom-Json
    $runtimeManifest = Get-Content -LiteralPath (Join-Path $AppDirectory 'windows-runtime.json') -Raw | ConvertFrom-Json
    Check ($backendManifest.package_profile -eq $RuntimeProfile -and $runtimeManifest.profile -eq $RuntimeProfile) 'staged package declares the explicitly selected runtime profile'
    Check (!$backendManifest.standalone_requires_ros -and !$backendManifest.standalone_requires_wsl -and
        !$backendManifest.standalone_requires_python) 'standalone features declare no external ROS, WSL or Python runtime'
    $includeNativeHost = if ($RuntimeProfile -eq 'OptionalNativeRviz') { 1 } else { 0 }
    Check ([bool]$backendManifest.included -eq [bool]$includeNativeHost) 'optional Linux host inclusion matches the requested package profile'
    $report.payload_audit = $payload
    if ($Offscreen) {
        Check (Test-Path -LiteralPath (Join-Path $AppDirectory 'platforms/qoffscreen.dll') -PathType Leaf) 'offscreen verification uses the packaged official Qt platform plugin'
    }
    # Generate a fixture identity outside the production payload; never relabel or
    # mutate its manifest just to run an isolated acceptance installer.
    $sourceManifest = Join-Path $AppDirectory 'uav-studio-install.json'
    $sourceManifestHash = if (Test-Path -LiteralPath $sourceManifest) { (Get-FileHash -LiteralPath $sourceManifest).Hash } else { $null }
    $manifest = Write-WindowsInstallManifest $AppDirectory $productId $Version $product 'uav-studio.exe' (Join-Path $ArtifactDirectory 'fixture-install-manifest.json')
    $arguments = @('/WX', "/DAPP_DIR=$AppDirectory", "/DSETUP_FILE=$setup", "/DVERSION=$Version", "/DDISPLAY_VERSION=$DisplayVersion",
        "/DINSTALL_MANIFEST_SHA256=$($manifest.sha256)", "/DINSTALL_MANIFEST_FILE=$($manifest.path)",
        "/DDIAGNOSTIC_LOG=$(Join-Path $ArtifactDirectory 'installer-helper.log')",
        "/DAUTHOR=$($metadata.author)", "/DPROJECT_URL=$($metadata.github_url)", "/DCONTACT_EMAIL=$($metadata.email)",
        "/DINCLUDE_NATIVE_RVIZ_HOST=$includeNativeHost",
        "/DPRODUCT=$product", "/DPRODUCT_ID=$productId", "/DAPP_EXE=$($metadata.executable_name).exe", "/DASSOCIATION_ROOT=$associationRoot",
        "/DSTARTMENU_DIRECTORY=$start", "/DDESKTOP_DIRECTORY=$desktop", (Join-Path $repository 'native/installer/windows.nsi'))
    RunProgram $NsisCompiler $arguments 'compile' 180
    Check (Test-Path -LiteralPath $setup) 'complete payload compiles into an isolated installer'
    Check (($sourceManifestHash -and (Get-FileHash -LiteralPath $sourceManifest).Hash -eq $sourceManifestHash) -or
        (!$sourceManifestHash -and !(Test-Path -LiteralPath $sourceManifest))) 'isolated acceptance does not change the production payload manifest'
    $process = Start-Process -FilePath $setup -ArgumentList '/S', '/DESKTOPSHORTCUT=1', "/D=$install" -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $ArtifactDirectory 'install.out.log') -RedirectStandardError (Join-Path $ArtifactDirectory 'install.err.log')
    $report.install_exit_code=$process.ExitCode
    Check ($process.ExitCode -eq 0) "complete isolated installation succeeds (actual $($process.ExitCode))"
    Check ((Get-FileHash -LiteralPath (Join-Path $installedApp 'uav-studio-install.json')).Hash -eq $manifest.sha256) 'installed ownership manifest matches the compiler-pinned isolated fixture identity'
    Check ((Get-ItemPropertyValue -LiteralPath $uninstallRoot -Name DisplayVersion) -eq $DisplayVersion) 'installed preview version label matches the package'
    Check ((Get-Item -LiteralPath $setup).VersionInfo.FileVersion -eq $DisplayVersion) 'installer file version retains the preview label'

    $files = @(Get-ChildItem -LiteralPath $AppDirectory -File -Recurse)
    $mismatches = @()
    foreach ($file in $files) {
        $relative = [IO.Path]::GetRelativePath($AppDirectory, $file.FullName)
        $installedFile = Join-Path $installedApp $relative
        $expectedFile = if ($relative -eq 'uav-studio-install.json') { $manifest.path } else { $file.FullName }
        if (!(Test-Path -LiteralPath $installedFile -PathType Leaf) -or
            (Get-FileHash -LiteralPath $expectedFile).Hash -ne (Get-FileHash -LiteralPath $installedFile).Hash) {
            $mismatches += $relative
        }
    }
    $report.payload_files = $files.Count
    $report.payload_mismatches = $mismatches
    Check ($files.Count -gt 100 -and $mismatches.Count -eq 0) 'every installed payload file matches the staged SHA-256'
    foreach ($relative in @('uav-studio.exe', 'uav-studio-cli.exe', 'platforms/qwindows.dll', 'imageformats/qtiff.dll',
        'ffmpeg.exe', 'ffprobe.exe', 'licenses/FFmpeg.txt', 'licenses/FFmpeg-build-README.txt',
        'licenses/FFmpeg-version.txt', 'licenses/FFmpeg-buildconf.txt', 'licenses/Qt-LGPL-3.0-only.txt',
        'licenses/opencv-LICENSE.txt', 'licenses/opencv-ffmpeg-LICENSE.txt', 'licenses/yaml-cpp.txt',
        'native-rviz-backend.json', 'windows-runtime.json')) {
        Check (Test-Path -LiteralPath (Join-Path $installedApp $relative)) "installed dependency/notice: $relative"
    }
    foreach ($folder in @($start, $desktop)) {
        $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $folder "$product.lnk"))
        Check ($shortcut.TargetPath -eq (Join-Path $installedApp 'uav-studio.exe')) 'isolated shortcut targets the complete installed application'
    }
    $backendDirectory = Join-Path $installedApp 'rviz_host'
    $report.native_rviz_backend_included = Test-Path -LiteralPath $backendDirectory
    $report.native_rviz_external_runtime_verified = $false
    Check ($report.native_rviz_backend_included -eq ($RuntimeProfile -eq 'OptionalNativeRviz')) 'installed optional backend matches the explicit runtime profile'
    if ($report.native_rviz_backend_included) {
        foreach ($relative in @('uav-studio-rviz-host', 'run_host.sh', 'README.md', 'README_CN.md', 'CMakeLists.txt',
            'main.cpp', 'bag_replay.h', 'bag_replay.cpp', 'header_layout.h', 'tests/header_layout_test.cpp', 'LICENSE', 'backend.json')) {
            Check (Test-Path -LiteralPath (Join-Path $backendDirectory $relative)) "installed Linux RViz backend: $relative"
        }
        $elf = Get-NativeRvizElfInfo (Join-Path $backendDirectory 'uav-studio-rviz-host')
        $backend = Get-Content -LiteralPath (Join-Path $backendDirectory 'backend.json') -Raw | ConvertFrom-Json
        Check ($elf.sha256 -eq $backend.sha256 -and !$backend.windows_native_plugins -and
            $backend.windows_launcher.StartsWith('wsl.exe')) 'installed backend is the declared Linux ELF, launched through WSL'
        $launcher = Get-Content -LiteralPath (Join-Path $backendDirectory 'run_host.sh') -Raw
        Check ($launcher.Contains('$script_dir/uav-studio-rviz-host')) 'installed launcher resolves its adjacent ELF without a source checkout'
        Check (!$launcher.Contains("`r") -and [IO.File]::ReadAllBytes((Join-Path $backendDirectory 'run_host.sh'))[0] -eq 35) 'installed Bash launcher has LF endings and no UTF-8 BOM'
    }

    $env:PATH = "$installedApp;$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($name in $savedEnvironment.Keys | Where-Object { $_ -ne 'PATH' }) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    if ($Offscreen) {
        $env:QT_QPA_PLATFORM = 'offscreen'
        # The Windows offscreen plugin uses FreeType instead of the Windows
        # font database. Use installed system fonts for legible smoke images.
        $env:QT_QPA_FONTDIR = Join-Path $env:SystemRoot 'Fonts'
    }
    # A Windows GUI subsystem build otherwise routes Qt diagnostics to the debugger,
    # leaving redirected stderr empty when a smoke check fails.
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $exe = Join-Path $installedApp 'uav-studio.exe'
    $cli = Join-Path $installedApp 'uav-studio-cli.exe'
    RunProgram $cli @('--help') 'cli-help'
    RunProgram $exe @('--smoke-layout', (Join-Path $ArtifactDirectory 'installed-layout.png')) 'qml-layout' -NativeWindow:(!$Offscreen)
    RunProgram $exe @('--smoke-png') 'transparent-png' -NativeWindow:(!$Offscreen)
    RunProgram $exe @('--smoke-mp4') 'h264-aac-export' 55 -NativeWindow:(!$Offscreen)
    $smokeArguments = @('--smoke')
    if ($VideoFile) { $smokeArguments += $VideoFile }
    RunProgram $exe $smokeArguments 'project-video-decode-edit' 45 -NativeWindow:(!$Offscreen)
    $png = Join-Path $ArtifactDirectory 'installed-layout.png'
    if ($Offscreen) {
        # Qt's Windows offscreen plugin reports RhiBasedRendering=false and has
        # no OpenGL context factory. Do not call its decoding smoke proof of 3D
        # rendering, or relax the exporter's rejection of an unrendered scene.
        $report.skipped_checks += [ordered]@{name='CLI persistent 3D PNG';
            reason='Windows Qt offscreen does not provide the required OpenGL rendering. Run the native-window check for this gate.'}
    } else {
        $png = Join-Path $ArtifactDirectory 'installed-demo.png'
        RunProgram $cli @('demo', '--start', '0.25', '--output', $png) 'cli-render' -NativeWindow
        Check (Test-Path -LiteralPath $png) 'CLI produces a persistent PNG from the installed renderer'
        $report.opengl_output_verified=$true
    }
    RunProgram (Join-Path $installedApp 'ffprobe.exe') @('-v', 'error', '-show_entries', 'stream=codec_name,width,height', '-of', 'json', $png) 'png-probe'
    $probe = Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'png-probe.out.log') -Raw | ConvertFrom-Json
    Check ($probe.streams.Count -eq 1 -and $probe.streams[0].codec_name -eq 'png' -and
        $probe.streams[0].width -gt 0 -and $probe.streams[0].height -gt 0) 'persistent smoke image has valid decoded dimensions'
    $report.png = $probe.streams[0]
    $report.png_role = if ($Offscreen) { 'Software QML layout smoke; not a 3D export' } else { 'CLI 3D export' }

    $process = Start-Process -FilePath (Join-Path $install 'Uninstall.exe') -ArgumentList '/S', "_?=$install" -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $ArtifactDirectory 'uninstall.out.log') -RedirectStandardError (Join-Path $ArtifactDirectory 'uninstall.err.log')
    Check ($process.ExitCode -eq 0 -and !(Test-Path -LiteralPath $exe)) 'complete isolated uninstall removes the installed application'
    Check (!(Test-Path -LiteralPath (Join-Path $start "$product.lnk")) -and
        !(Test-Path -LiteralPath (Join-Path $desktop "$product.lnk"))) 'complete uninstall removes isolated shortcuts'
    Check (!(Test-Path -LiteralPath $uninstallRoot)) 'complete uninstall removes its dedicated registration'
    $report.runnable_distribution_verified = !$Offscreen
    $report.offscreen_partial_runtime_verified = [bool]$Offscreen
    $report.success = $true
} catch {
    $report.error = $_.Exception.Message
    throw
} finally {
    foreach ($entry in $savedEnvironment.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process') }
    # A failed smoke must not leave its isolated acceptance installation behind.
    # This path was created by this run, never a production/user installation.
    $cleanupUninstaller = Join-Path $install 'Uninstall.exe'
    if (Test-Path -LiteralPath (Join-Path $installedApp 'uav-studio.exe')) {
        $report.failure_cleanup_attempted = $true
        try {
            $cleanup = Start-Process -FilePath $cleanupUninstaller -ArgumentList '/S', "_?=$install" -WindowStyle Hidden -PassThru -Wait `
                -RedirectStandardOutput (Join-Path $ArtifactDirectory 'failure-uninstall.out.log') -RedirectStandardError (Join-Path $ArtifactDirectory 'failure-uninstall.err.log')
            $report.failure_cleanup_exit = $cleanup.ExitCode
            $report.failure_cleanup_succeeded = $cleanup.ExitCode -eq 0 -and !(Test-Path -LiteralPath (Join-Path $installedApp 'uav-studio.exe'))
        } catch { $report.failure_cleanup_error = $_.Exception.Message }
    }
    if ($productId -notmatch '^UAVStudioRuntimeAcceptance[0-9a-f]{12}$') { throw 'Unsafe acceptance registry namespace.' }
    $acceptanceKey = "HKCU:\Software\UAVStudioRuntimeAcceptance\$runId"
    if (Test-Path -LiteralPath $acceptanceKey) { Remove-Item -LiteralPath $acceptanceKey -Recurse -Force }
    if (Test-Path -LiteralPath $uninstallRoot) { Remove-Item -LiteralPath $uninstallRoot -Recurse -Force }
    $report | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $ArtifactDirectory 'runtime-check.json')
}
Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'runtime-check.json')
