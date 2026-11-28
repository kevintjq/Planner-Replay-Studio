param(
    [string]$NativeRvizHostBinary,
    [string]$ArtifactDirectory
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
. "$repository/native/installer/native-rviz-host.ps1"
if (!$ArtifactDirectory) { $ArtifactDirectory = Join-Path $repository ("captures/windows-payload-guard-" + [Guid]::NewGuid().ToString('N')) }
$ArtifactDirectory = [IO.Path]::GetFullPath($ArtifactDirectory)
if (!$ArtifactDirectory.StartsWith($repository + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Payload regression artifacts must remain inside this repository.'
}
if (Test-Path -LiteralPath $ArtifactDirectory) { throw 'Choose a fresh artifact directory.' }
New-Item -ItemType Directory -Path $ArtifactDirectory | Out-Null
$report = [ordered]@{kind='windows-runtime-payload-guard'; checks=@(); optional_elf_copy_verified=$false; runtime_execution_verified=$false; success=$false}
function Check([bool]$condition, [string]$name) {
    $script:report.checks += [ordered]@{name=$name;passed=$condition}
    if (!$condition) { throw "Payload regression failed: $name" }
}
function Expect-Rejection([scriptblock]$action, [string]$message, [string]$name) {
    $rejected = $false
    try { & $action | Out-Null } catch { $rejected = $_.Exception.Message -like $message }
    Check $rejected $name
}
function Fixture-Directory([string]$name) {
    $path = Join-Path $ArtifactDirectory $name
    New-Item -ItemType Directory -Path $path | Out-Null
    return $path
}
try {
    $fixtureRepository = Fixture-Directory 'developer-checkout'
    $candidate = Join-Path $fixtureRepository 'build-native/rviz-host/uav-studio-rviz-host'
    New-Item -ItemType Directory -Path (Split-Path -Parent $candidate) | Out-Null
    # An unrequested build artifact is intentionally not a runnable host.
    [IO.File]::WriteAllText($candidate, 'unrequested developer artifact')
    $standalone = Fixture-Directory 'standalone'
    $backend = Copy-NativeRvizHost $standalone $fixtureRepository $null
    Check (!$backend.included -and $backend.package_profile -eq 'Standalone' -and
        !$backend.standalone_requires_ros -and !$backend.standalone_requires_wsl -and
        !$backend.standalone_requires_python) 'default profile ignores an existing developer host build'
    Check (!(Test-Path -LiteralPath (Join-Path $standalone 'rviz_host'))) 'default profile creates no Linux host directory'
    [IO.File]::WriteAllText((Join-Path $standalone 'uav-studio.exe'), 'Windows payload scan fixture, not an executable')
    $payload = Assert-WindowsRuntimePayload $standalone
    Check ($payload.profile -eq 'Standalone' -and !$payload.clean_windows_without_wsl_verified) 'payload audit states its clean OS verification limit'

    $stale = Fixture-Directory 'stale'
    New-Item -ItemType Directory -Path (Join-Path $stale 'rviz_host') | Out-Null
    Expect-Rejection { Copy-NativeRvizHost $stale $repository $null } '*refuses a stale rviz_host*' 'default profile refuses a stale Linux host directory'
    Expect-Rejection { Assert-WindowsRuntimePayload $stale } '*must not contain*' 'standalone payload guard rejects the optional host directory'

    $invalid = Fixture-Directory 'invalid-explicit'
    Expect-Rejection { Copy-NativeRvizHost $invalid $repository $candidate } '*Linux x86_64 ELF*' 'an explicitly supplied non-ELF host is rejected'
    Check (!(Test-Path -LiteralPath (Join-Path $invalid 'rviz_host'))) 'invalid explicit host leaves staging unmodified'

    foreach ($name in @('python313.dll', 'pythonw.exe', 'worker.py', 'extension.pyd', 'roscore.exe', 'rviz2.exe', 'roscpp.dll', 'rclcpp_custom.dll', 'wsl.exe', 'foreign.so')) {
        $caseDirectory = Fixture-Directory ('forbidden-' + $name.Replace('.', '-'))
        $file = Join-Path $caseDirectory $name
        if ($name -eq 'foreign.so') { [IO.File]::WriteAllBytes($file, [byte[]]@(0x7f,0x45,0x4c,0x46,0,0,0,0)) }
        else { [IO.File]::WriteAllText($file, 'unexpected dependency fixture') }
        Expect-Rejection { Assert-WindowsRuntimePayload $caseDirectory } '*unexpected Python/ROS/WSL/Linux runtime files*' "rejects unbundled runtime category: $name"
    }
    if ($NativeRvizHostBinary) {
        $optional = Fixture-Directory 'explicit-optional'
        $info = Copy-NativeRvizHost $optional $repository $NativeRvizHostBinary
        $copied = Get-NativeRvizElfInfo (Join-Path $optional 'rviz_host/uav-studio-rviz-host')
        Check ($info.included -and $info.optional -and $info.package_profile -eq 'OptionalNativeRviz' -and
            !$info.windows_native_plugins -and $copied.sha256 -eq $info.sha256) 'explicit optional Linux ELF copies with its actual hash and honest platform declaration'
        $optionalPayload = Assert-WindowsRuntimePayload $optional $true
        Check ($optionalPayload.profile -eq 'OptionalNativeRviz') 'explicit optional host stays isolated from the Windows runtime'
        $launcher = [IO.File]::ReadAllText((Join-Path $optional 'rviz_host/run_host.sh'))
        Check (!$launcher.Contains("`r") -and [IO.File]::ReadAllBytes((Join-Path $optional 'rviz_host/run_host.sh'))[0] -eq 35) 'optional Bash launcher remains LF without a BOM'
        foreach ($relative in @('README.md', 'CMakeLists.txt', 'main.cpp', 'bag_replay.h', 'bag_replay.cpp',
            'header_layout.h', 'tests/header_layout_test.cpp')) {
            $source = Join-Path $repository ('native/rviz_host/' + $relative)
            $packaged = Join-Path $optional ('rviz_host/' + $relative)
            Check ((Test-Path -LiteralPath $packaged -PathType Leaf) -and
                (Get-FileHash -LiteralPath $source).Hash -ceq (Get-FileHash -LiteralPath $packaged).Hash) "optional Host rebuild source is complete and byte-identical: $relative"
        }
        $hostCmake = [IO.File]::ReadAllText((Join-Path $optional 'rviz_host/CMakeLists.txt'))
        $hostCmakeSources = @([regex]::Matches($hostCmake, '(?<![A-Za-z0-9_./-])([A-Za-z0-9_./-]+\.(?:cpp|h))') | ForEach-Object {$_.Groups[1].Value} | Sort-Object -Unique)
        $missingHostSources = @($hostCmakeSources | Where-Object { !(Test-Path -LiteralPath (Join-Path $optional ('rviz_host/' + $_)) -PathType Leaf) })
        Check ($hostCmakeSources.Count -ge 3 -and $missingHostSources.Count -eq 0) 'optional Host payload closes all CMake production and default CTest source references'
        $report.optional_elf_copy_verified = $true
    }
    $report.success = $true
} catch {
    $report.error = $_.Exception.Message
    throw
} finally {
    $report | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $ArtifactDirectory 'report.json')
}
Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'report.json')
