function Get-NativeRvizElfInfo([string]$Binary) {
    $Binary = (Resolve-Path -LiteralPath $Binary).Path
    $stream = [IO.File]::OpenRead($Binary)
    try {
        $header = New-Object byte[] 20
        if ($stream.Read($header, 0, $header.Length) -ne $header.Length -or
            $header[0] -ne 0x7f -or $header[1] -ne 0x45 -or $header[2] -ne 0x4c -or $header[3] -ne 0x46 -or
            $header[4] -ne 2 -or $header[5] -ne 1 -or $header[6] -ne 1 -or
            $header[18] -ne 62 -or $header[19] -ne 0 -or $header[17] -ne 0 -or $header[16] -notin @(2, 3)) {
            throw 'Native RViz host must be a little-endian Linux x86_64 ELF executable, not a Windows PE or another architecture.'
        }
    } finally { $stream.Dispose() }
    return [ordered]@{format = 'ELF64'; architecture = 'x86_64'; operating_system = 'Linux'; sha256 = (Get-FileHash -LiteralPath $Binary -Algorithm SHA256).Hash; bytes = (Get-Item -LiteralPath $Binary).Length}
}

function Copy-NativeRvizHost([string]$AppDirectory, [string]$RepositoryRoot, [string]$NativeRvizHostBinary) {
    $hostDirectory = Join-Path $AppDirectory 'rviz_host'
    if (!$NativeRvizHostBinary) {
        # Developer build artifacts must never change the default Windows product.
        if (Test-Path -LiteralPath $hostDirectory) { throw 'Standalone packaging refuses a stale rviz_host payload. Use clean staging, or explicitly supply -NativeRvizHostBinary for the optional Linux backend.' }
        return [ordered]@{
            included = $false; optional = $true; package_profile = 'Standalone'
            reason = 'Optional Linux RViz backend was not explicitly requested'
            windows_native_plugins = $false; external_runtime = @()
            standalone_requires_ros = $false; standalone_requires_wsl = $false; standalone_requires_python = $false
        }
    }
    # Validate the real artifact before changing the payload.
    $info = Get-NativeRvizElfInfo $NativeRvizHostBinary
    $sourceDirectory = Join-Path $RepositoryRoot 'native/rviz_host'
    $sources = @('run_host.sh', 'README.md', 'CMakeLists.txt', 'main.cpp', 'bag_replay.h', 'bag_replay.cpp',
        'header_layout.h', 'tests/header_layout_test.cpp')
    foreach ($file in $sources) {
        if (!(Test-Path -LiteralPath (Join-Path $sourceDirectory $file) -PathType Leaf)) { throw "Missing native RViz rebuild/launcher file: $file" }
    }
    foreach ($file in @('LICENSE', 'docs/NATIVE_RVIZ_BACKEND_CN.md')) {
        if (!(Test-Path -LiteralPath (Join-Path $RepositoryRoot $file) -PathType Leaf)) { throw "Missing native RViz notice: $file" }
    }
    New-Item -ItemType Directory -Force -Path $hostDirectory | Out-Null
    Copy-Item -LiteralPath $NativeRvizHostBinary -Destination (Join-Path $hostDirectory 'uav-studio-rviz-host')
    foreach ($file in $sources) {
        $destination = Join-Path $hostDirectory $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath (Join-Path $sourceDirectory $file) -Destination $destination
    }
    # Windows Git checkouts may use CRLF; Bash must receive LF without a BOM.
    $launcherPath = Join-Path $hostDirectory 'run_host.sh'
    $launcherText = [IO.File]::ReadAllText($launcherPath).Replace("`r`n", "`n")
    [IO.File]::WriteAllText($launcherPath, $launcherText, [Text.UTF8Encoding]::new($false))
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'LICENSE') -Destination (Join-Path $hostDirectory 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'docs/NATIVE_RVIZ_BACKEND_CN.md') -Destination (Join-Path $hostDirectory 'README_CN.md')
    $info.included = $true
    $info.optional = $true
    $info.package_profile = 'OptionalNativeRviz'
    $info.windows_native_plugins = $false
    $info.standalone_requires_ros = $false
    $info.standalone_requires_wsl = $false
    $info.standalone_requires_python = $false
    $info.windows_launcher = 'wsl.exe -> configured Linux distribution -> bash run_host.sh'
    $info.external_runtime = @('Compatible Ubuntu 20.04 Linux x86_64 / WSL', 'ROS 1 Noetic and RViz with requested plugins', 'Qt5 and OGRE/OpenGL', '/opt/ros/noetic/setup.bash or a compatible explicit setup')
    $info | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $hostDirectory 'backend.json')
    return $info
}

function Assert-WindowsRuntimePayload([string]$AppDirectory, [bool]$AllowNativeRvizHost = $false) {
    $AppDirectory = (Resolve-Path -LiteralPath $AppDirectory).Path
    $hostDirectory = Join-Path $AppDirectory 'rviz_host'
    if (!$AllowNativeRvizHost -and (Test-Path -LiteralPath $hostDirectory)) {
        throw 'Standalone Windows payload must not contain the optional Linux rviz_host directory.'
    }
    $files = @(Get-ChildItem -LiteralPath $AppDirectory -File -Recurse)
    $payloadPrefix = $AppDirectory.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $forbidden = @()
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($payloadPrefix.Length)
        if ($file.Extension -in @('.py', '.pyc', '.pyo', '.pyd', '.pyw') -or
            $file.Name -match '^(python(?:w)?[0-9.]*|libpython[0-9.]*)\.(dll|exe)$' -or
            $file.Name -match '^(wsl|roscore|rosmaster|roslaunch|rosbag|ros2|rviz|rviz2)\.exe$' -or
            $file.Name -match '^(?:lib)?(?:roscpp|rosconsole|rostime|rosbag|rospack|rclcpp|rcl|rmw|rviz)(?:[_-].*)?\.dll$') {
            $forbidden += $relative
            continue
        }
        # Optional Linux host stays isolated; no other ELF library may leak into the Windows payload.
        if ($AllowNativeRvizHost -and $relative.StartsWith('rviz_host' + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $stream = [IO.File]::OpenRead($file.FullName)
        try {
            $header = New-Object byte[] 4
            if ($stream.Read($header, 0, 4) -eq 4 -and $header[0] -eq 0x7f -and
                $header[1] -eq 0x45 -and $header[2] -eq 0x4c -and $header[3] -eq 0x46) { $forbidden += $relative }
        } finally { $stream.Dispose() }
    }
    if ($forbidden.Count) { throw "Windows payload contains unexpected Python/ROS/WSL/Linux runtime files: $($forbidden -join ', ')" }
    return [ordered]@{
        profile = $(if ($AllowNativeRvizHost) { 'OptionalNativeRviz' } else { 'Standalone' })
        payload_files = $files.Count; forbidden_runtime_files = @()
        python_runtime_included = $false; ros_windows_runtime_included = $false; wsl_launcher_included = $false
        # This is a payload audit, not proof that all code paths ran on a clean OS.
        clean_windows_without_wsl_verified = $false
    }
}
