param([string]$ArtifactDirectory)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
if (!$ArtifactDirectory) { $ArtifactDirectory = Join-Path $repository ("captures/windows-packaging-static-" + [Guid]::NewGuid().ToString('N')) }
$ArtifactDirectory = [IO.Path]::GetFullPath($ArtifactDirectory)
if (!$ArtifactDirectory.StartsWith($repository + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Static packaging artifacts must remain inside this repository.'
}
if (Test-Path -LiteralPath $ArtifactDirectory) { throw 'Choose a fresh artifact directory.' }
New-Item -ItemType Directory -Path $ArtifactDirectory | Out-Null
$report = [ordered]@{kind='windows-packaging-static'; checks=@(); application_executed=$false; cmake_configured=$false; runtime_verified=$false; success=$false}
function Check([bool]$condition, [string]$name) {
    $script:report.checks += [ordered]@{name=$name;passed=$condition}
    if (!$condition) { throw "Static packaging check failed: $name" }
}
try {
    $cmake = Get-Content -LiteralPath (Join-Path $repository 'native/CMakeLists.txt') -Raw
    $runtimeCmake = ($cmake -split 'include\(CTest\)', 2)[0]
    $sourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $repository 'native/src') -File -Filter '*.cpp')
    $unlistedSources = @($sourceFiles | Where-Object { !$runtimeCmake.Contains('src/' + $_.Name) } | ForEach-Object Name)
    Check ($unlistedSources.Count -eq 0) 'all runtime C++ translation units are in the production CMake graph'
    $literalPaths = @([regex]::Matches($cmake, '(?<![A-Za-z0-9_])((?:src|tests|tools|resources|qml)/[A-Za-z0-9_./-]+\.(?:cpp|h|qml|rc|desktop|xml)(?:\.in)?)') | ForEach-Object {$_.Groups[1].Value} | Sort-Object -Unique)
    $missingPaths = @($literalPaths | Where-Object { !(Test-Path -LiteralPath (Join-Path $repository ('native/' + $_)) -PathType Leaf) })
    Check ($missingPaths.Count -eq 0) 'all literal CMake source, test, resource and desktop paths exist'
    $interface = [regex]::Match($runtimeCmake, 'qt_add_resources\(UAVStudio\s+"interface"\s+PREFIX\s+"/"\s+FILES\s+([\s\S]*?)\)')
    Check $interface.Success 'production interface resources are explicitly embedded in the application'
    $qmlResources = @([regex]::Matches($interface.Groups[1].Value, 'qml/[A-Za-z0-9_-]+\.qml') | ForEach-Object {$_.Value})
    $qmlFiles = @(Get-ChildItem -LiteralPath (Join-Path $repository 'native/qml') -File -Filter '*.qml')
    $unembeddedQml = @($qmlFiles | Where-Object { ('qml/' + $_.Name) -notin $qmlResources } | ForEach-Object Name)
    Check ($unembeddedQml.Count -eq 0) 'every application QML file is embedded, including the new shared canvas and Inspectors'
    Check (($qmlResources | Sort-Object -Unique).Count -eq $qmlResources.Count) 'QML resource list contains no duplicate entries'
    Check ($runtimeCmake.Contains('install(TARGETS UAVStudio UAVStudioCli RUNTIME DESTINATION .)')) 'Windows CMake installs only the two product targets, not developer tests or asset tools'
    foreach ($asset in @('uav-studio.svg','uav-studio.ico','uav-studio.icns','uav-studio-16.png','uav-studio-32.png','uav-studio-48.png','uav-studio-64.png','uav-studio-128.png','uav-studio-256.png','uav-studio-512.png','uav-studio-1024.png')) {
        Check (Test-Path -LiteralPath (Join-Path $repository ('assets/icons/' + $asset)) -PathType Leaf) "branding resource exists: $asset"
    }
    $package = Get-Content -LiteralPath (Join-Path $repository 'native/installer/package.ps1') -Raw
    Check ($package.IndexOf('Assert-FreshWindowsStaging $AppDirectory $FFmpegDirectory') -gt 0 -and
        $package.IndexOf('Assert-FreshWindowsStaging $AppDirectory $FFmpegDirectory') -lt $package.IndexOf('$opencvDlls | Copy-Item')) 'fresh staging is checked before dependency deployment mutates the directory'
    Check ($package.Contains('--qmldir "$root/native/qml"') -and $package.Contains('imageformats/qtiff.dll')) 'Qt deployment scans production QML and requires the TIFF image plugin'
    Check ($package.Contains('plugins/platforms/qoffscreen.dll') -and $package.Contains('platforms/qoffscreen.dll')) 'unattended offscreen checks use a plugin deployed from the selected Qt distribution'
    Check ($package.Contains('Copy-WindowsDocumentation $AppDirectory $root') -and $package.Contains("'documentation.json'")) 'packaging preserves the validated offline documentation layout and records its manifest'
    $installer = Get-Content -LiteralPath (Join-Path $repository 'native/installer/windows.nsi') -Raw
    Check ($installer.Contains('!define INCLUDE_NATIVE_RVIZ_HOST 0') -and $installer.Contains('File /r /x "rviz_host"')) 'NSIS excludes the Linux host by default even when invoked directly'
    $prepare = $installer.IndexOf('StrCpy $R9 "Prepare"')
    $extract = $installer.IndexOf('SetOutPath "$UpgradeStaging\app"')
    $commit = $installer.IndexOf('StrCpy $R9 "Commit"')
    $register = $installer.IndexOf('WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName"')
    Check ($prepare -ge 0 -and $prepare -lt $extract -and $extract -lt $commit -and $commit -lt $register -and
        $installer.Contains('Call RunUpgradeHelper') -and $installer.Contains('SetOutPath "$PLUGINSDIR"')) 'trusted-root preparation, complete staging and verified directory replacement precede registration'
    Check ($installer.Contains('!ifndef INSTALL_MANIFEST_SHA256') -and $package.Contains('Write-WindowsInstallManifest') -and
        $package.Contains('"/DCONTACT_EMAIL=$($metadata.email)"') -and $package.Contains('$identity.CompanyName -cne $metadata.author')) 'installer uses a required hashed payload manifest and validates central metadata against both executable resources'
    foreach ($script in @('native/installer/package.ps1','native/installer/windows-package.ps1','native/installer/windows-upgrade.ps1','native/installer/windows-documentation.ps1','native/installer/native-rviz-host.ps1','native/tools/test-windows-staging-input.ps1','native/tools/test-windows-upgrade.ps1','native/tools/new-brand-installer-check.ps1','native/tools/test-windows-documentation.ps1','native/tools/test-windows-runtime-payload.ps1','native/tools/new-package-runtime-check.ps1','native/tools/test-release-gates.ps1')) {
        $tokens = $null
        $errors = $null
        [Management.Automation.Language.Parser]::ParseFile((Join-Path $repository $script), [ref]$tokens, [ref]$errors) | Out-Null
        Check ($errors.Count -eq 0) "PowerShell parser accepts $script"
    }
    $report.runtime_cpp_count = $sourceFiles.Count
    $report.qml_resource_count = $qmlResources.Count
    $report.checked_cmake_paths = $literalPaths.Count
    $report.unlisted_sources = $unlistedSources
    $report.unembedded_qml = $unembeddedQml
    $report.missing_cmake_paths = $missingPaths
    $report.success = $true
} catch {
    $report.error = $_.Exception.Message
    throw
} finally {
    $report | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $ArtifactDirectory 'report.json')
}
Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'report.json')
