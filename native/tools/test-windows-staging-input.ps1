param([string]$ArtifactDirectory)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
. "$repository/native/installer/windows-package.ps1"
if (!$ArtifactDirectory) { $ArtifactDirectory = Join-Path $repository ("captures/windows-staging-input-" + [Guid]::NewGuid().ToString('N')) }
$ArtifactDirectory = [IO.Path]::GetFullPath($ArtifactDirectory)
if (!$ArtifactDirectory.StartsWith($repository + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Staging regression artifacts must remain inside this repository.'
}
if (Test-Path -LiteralPath $ArtifactDirectory) { throw 'Choose a fresh artifact directory.' }
New-Item -ItemType Directory -Path $ArtifactDirectory | Out-Null
$report = [ordered]@{kind='fresh-windows-staging-input'; checks=@(); application_executed=$false; success=$false}
function Check([bool]$condition, [string]$name) {
    $script:report.checks += [ordered]@{name=$name;passed=$condition}
    if (!$condition) { throw "Staging regression failed: $name" }
}
function FreshFixture([string]$name) {
    $directory = Join-Path $ArtifactDirectory $name
    New-Item -ItemType Directory -Path $directory | Out-Null
    # These text files deliberately cannot execute; only input hygiene is tested.
    foreach ($program in @('uav-studio.exe', 'uav-studio-cli.exe')) {
        [IO.File]::WriteAllText((Join-Path $directory $program), 'Non-runnable packaging fixture')
    }
    return $directory
}
function RejectFixture([string]$directory, [string]$name) {
    $before = @(Get-ChildItem -LiteralPath $directory -Force -Recurse | ForEach-Object FullName)
    $rejected = $false
    try { Assert-FreshWindowsStaging $directory $script:ffmpeg | Out-Null } catch { $rejected = $true }
    Check $rejected $name
    $after = @(Get-ChildItem -LiteralPath $directory -Force -Recurse | ForEach-Object FullName)
    Check (($before -join '|') -ceq ($after -join '|')) "$name preserves all input entries"
}
try {
    $ffmpeg = Join-Path $ArtifactDirectory 'selected-ffmpeg'
    New-Item -ItemType Directory -Path (Join-Path $ffmpeg 'bin') | Out-Null
    [IO.File]::WriteAllText((Join-Path $ffmpeg 'bin/avcodec-61.dll'), 'Selected FFmpeg ABI fixture')
    $fresh = FreshFixture 'fresh-cmake-install'
    Copy-Item -LiteralPath (Join-Path $ffmpeg 'bin/avcodec-61.dll') -Destination $fresh
    $inputReport = Assert-FreshWindowsStaging $fresh $ffmpeg
    Check ($inputReport.checked_existing_files -eq 3 -and $inputReport.files_removed -eq 0) 'fresh CMake programs and matching FFmpeg DLL are accepted'
    $programsOnly = FreshFixture 'programs-before-dependency-deploy'
    Check ((Assert-FreshWindowsStaging $programsOnly $ffmpeg).checked_existing_files -eq 2) 'programs-only input permits dependencies to be deployed by package.ps1'
    foreach ($name in @('Qt6Core.dll', 'avcodec-60.dll', 'canvas_binding_test.exe', 'private.bag', 'private.mp4', 'README.md', '.hidden-stale')) {
        $directory = FreshFixture ('stale-' + $name.Replace('.', '-'))
        [IO.File]::WriteAllText((Join-Path $directory $name), 'Preserve this rejected stale entry')
        RejectFixture $directory "rejects reused staging entry $name"
    }
    foreach ($name in @('rviz_host', 'imageformats', 'old-output')) {
        $directory = FreshFixture ('stale-directory-' + $name)
        New-Item -ItemType Directory -Path (Join-Path $directory $name) | Out-Null
        RejectFixture $directory "rejects reused staging directory $name"
    }
    $wrongAbi = FreshFixture 'same-name-wrong-ffmpeg'
    [IO.File]::WriteAllText((Join-Path $wrongAbi 'avcodec-61.dll'), 'Different build with the same DLL name')
    RejectFixture $wrongAbi 'rejects a same-name codec DLL whose bytes differ from selected FFmpeg'
    $missing = Join-Path $ArtifactDirectory 'missing-cli'
    New-Item -ItemType Directory -Path $missing | Out-Null
    [IO.File]::WriteAllText((Join-Path $missing 'uav-studio.exe'), 'Non-runnable packaging fixture')
    RejectFixture $missing 'rejects missing CLI before deployment'
    $manifestInput = FreshFixture 'install-manifest-input'
    New-Item -ItemType Directory -Path (Join-Path $manifestInput 'nested') | Out-Null
    [IO.File]::WriteAllText((Join-Path $manifestInput 'nested/notice.txt'),'Manifest nested file fixture.')
    $manifest = Write-WindowsInstallManifest $manifestInput 'UAVStudioManifestFixture' '0.1.1'
    $document = Get-Content -LiteralPath $manifest.path -Raw | ConvertFrom-Json
    Check ($document.files.Count -eq 3 -and $document.install_id -eq 'UAVStudioManifestFixture' -and
        $document.files.path -contains 'nested/notice.txt' -and $document.files.path -notcontains 'uav-studio-install.json') 'manifest identifies every payload file and excludes only its own root document'
    $mismatches = @($document.files | Where-Object {
        $path=Join-Path $manifestInput $_.path
        (Get-Item -LiteralPath $path).Length -ne $_.bytes -or (Get-FileHash -LiteralPath $path).Hash -ne $_.sha256
    })
    Check ($mismatches.Count -eq 0 -and (Get-FileHash -LiteralPath $manifest.path).Hash -eq $manifest.sha256) 'manifest records exact byte lengths and SHA-256 values'
    $same = Write-WindowsInstallManifest $manifestInput 'UAVStudioManifestFixture' '0.1.1'
    Check ($same.sha256 -eq $manifest.sha256) 'unchanged payload regenerates the same manifest hash'
    $external = Write-WindowsInstallManifest $manifestInput 'UAVStudioIsolatedFixture' '0.1.0' 'Fixture' 'uav-studio.exe' (Join-Path $ArtifactDirectory 'external-install-manifest.json')
    Check ($external.install_id -eq 'UAVStudioIsolatedFixture' -and (Get-FileHash -LiteralPath $manifest.path).Hash -eq $manifest.sha256) 'external fixture manifest preserves original payload identity and bytes'
    $reparseInput = FreshFixture 'reparse-manifest-input'
    $junction = Join-Path $reparseInput 'linked-payload'
    New-Item -ItemType Junction -Path $junction -Target $manifestInput | Out-Null
    $rejected = $false
    try { Write-WindowsInstallManifest $reparseInput 'UAVStudioManifestFixture' '0.1.1' | Out-Null } catch { $rejected = $true }
    Check ($rejected -and !(Test-Path -LiteralPath (Join-Path $reparseInput 'uav-studio-install.json'))) 'manifest refuses a reparse payload entry without creating an ownership document'
    $report.success = $true
} catch {
    $report.error = $_.Exception.Message
    throw
} finally {
    $report | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $ArtifactDirectory 'report.json')
}
Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'report.json')
