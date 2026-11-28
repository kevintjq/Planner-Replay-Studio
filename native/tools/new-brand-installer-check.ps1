param(
    [Parameter(Mandatory = $true)][string]$NsisCompiler,
    [Parameter(Mandatory = $true)][string]$ApplicationExecutable,
    [string]$ArtifactDirectory
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
. "$repository/native/installer/windows-package.ps1"
$metadata = Get-Content -LiteralPath (Join-Path $repository 'app-metadata.json') -Raw | ConvertFrom-Json
$NsisCompiler = (Resolve-Path -LiteralPath $NsisCompiler).Path
$ApplicationExecutable = (Resolve-Path -LiteralPath $ApplicationExecutable).Path
$runId = [Guid]::NewGuid().ToString('N').Substring(0, 12)
if (!$ArtifactDirectory) { $ArtifactDirectory = Join-Path $repository "captures/branding-installer-$runId" }
$ArtifactDirectory = [System.IO.Path]::GetFullPath($ArtifactDirectory)
$allowedRoot = [System.IO.Path]::GetFullPath($repository) + [System.IO.Path]::DirectorySeparatorChar
if (!$ArtifactDirectory.StartsWith($allowedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Installer acceptance artifacts must remain inside this repository.'
}
if (Test-Path -LiteralPath $ArtifactDirectory) { throw 'Choose a fresh artifact directory.' }
New-Item -ItemType Directory -Path $ArtifactDirectory | Out-Null
$fixture = New-Item -ItemType Directory -Path (Join-Path $ArtifactDirectory 'fixture')
Copy-Item -LiteralPath $ApplicationExecutable -Destination (Join-Path $fixture.FullName 'uav-studio.exe')
Set-Content -Encoding utf8 (Join-Path $fixture.FullName 'README.txt') 'Installer integration fixture only. Runtime dependencies are intentionally not included; this is not an installable distribution.'

$product = "UAV Studio Installer Acceptance $runId"
$productId = "UAVStudioBrandingAcceptance$runId"
$projectId = "$productId.Project"
$associationRoot = "Software\UAVStudioInstallerAcceptance\$runId\Classes"
$registryRoot = "HKCU:\$associationRoot"
$uninstallRoot = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$productId"
$install = Join-Path $ArtifactDirectory 'isolated-install'
$desktop = Join-Path $ArtifactDirectory 'shortcuts/desktop'
$start = Join-Path $ArtifactDirectory 'shortcuts/start'
$setup = Join-Path $ArtifactDirectory 'installer-fixture.exe'
$report = [ordered]@{
    kind = 'isolated-installer-fixture'; production_registry_touched = $false
    runnable_distribution_verified = $false; checks = @(); success = $false
}
$script:installerRun = 0

function Check([bool]$condition, [string]$name) {
    $script:report.checks += [ordered]@{name = $name; passed = $condition}
    if (!$condition) { throw "Installer check failed: $name" }
}
function ReadDefault([string]$path) {
    if (!(Test-Path -LiteralPath $path)) { return $null }
    return (Get-Item -LiteralPath $path).GetValue('')
}
function RunInstaller([string]$shortcut, [string]$destination = $install, [int]$expectedExit = 0) {
    $script:installerRun = 1 + $script:installerRun
    $process = Start-Process -FilePath $setup -ArgumentList '/S', "/DESKTOPSHORTCUT=$shortcut", "/D=$destination" -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $ArtifactDirectory "install-$script:installerRun.out.log") `
        -RedirectStandardError (Join-Path $ArtifactDirectory "install-$script:installerRun.err.log")
    Check ($process.ExitCode -eq $expectedExit) "silent installation with desktop=$shortcut returns $expectedExit"
}

try {
    $manifest = Write-WindowsInstallManifest $fixture.FullName $productId '0.1.1' $product
    $arguments = @('/WX', "/DAPP_DIR=$($fixture.FullName)", "/DSETUP_FILE=$setup", '/DVERSION=0.1.1',
        "/DPRODUCT=$product", "/DPRODUCT_ID=$productId", "/DAPP_EXE=$($metadata.executable_name).exe", "/DASSOCIATION_ROOT=$associationRoot",
        "/DINSTALL_MANIFEST_SHA256=$($manifest.sha256)", "/DAUTHOR=$($metadata.author)", "/DPROJECT_URL=$($metadata.github_url)", "/DCONTACT_EMAIL=$($metadata.email)",
        "/DSTARTMENU_DIRECTORY=$start", "/DDESKTOP_DIRECTORY=$desktop", (Join-Path $repository 'native/installer/windows.nsi'))
    $quoted = $arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
    $compilerProcess = Start-Process -FilePath $NsisCompiler -ArgumentList $quoted -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $ArtifactDirectory 'compile.out.log') -RedirectStandardError (Join-Path $ArtifactDirectory 'compile.err.log')
    Check ($compilerProcess.ExitCode -eq 0 -and (Test-Path -LiteralPath $setup)) 'NSIS compilation with dedicated installer/uninstaller icons'

    # The test namespace never registers an actual Windows file association.
    New-Item -Path "$registryRoot\.slamfpv" -Force | Out-Null
    Set-Item -LiteralPath "$registryRoot\.slamfpv" -Value 'ExistingProjectOwner'
    RunInstaller '1'
    $exe = Join-Path $install 'app/uav-studio.exe'
    Check (Test-Path -LiteralPath $exe) 'installed executable exists'
    Check ((Get-FileHash -LiteralPath $exe).Hash -eq (Get-FileHash -LiteralPath $ApplicationExecutable).Hash) 'installed executable matches input'
    $registration = Get-ItemProperty -LiteralPath $uninstallRoot
    Check ($registration.Publisher -ceq $metadata.author -and $registration.Contact -ceq $metadata.email -and
        $registration.URLInfoAbout -ceq $metadata.github_url) 'installer author, contact and project URL derive from central metadata'
    Check ((ReadDefault "$registryRoot\.uavstudio") -eq $projectId) 'new project extension gets the new ProgID when unclaimed'
    Check ((ReadDefault "$registryRoot\.slamfpv") -eq 'ExistingProjectOwner') 'legacy project default remains owned by the existing application'
    Check ((Get-Item -LiteralPath "$registryRoot\.slamfpv\OpenWithProgids").GetValueNames() -contains $projectId) 'legacy project offers UAV Studio through Open With'
    $expectedCommand = '"' + $exe.Replace('/', '\') + '" "%1"'
    Check ((ReadDefault "$registryRoot\$projectId\shell\open\command") -eq $expectedCommand) 'file-open command quotes executable and project argument'
    Check ((ReadDefault "$registryRoot\$projectId\DefaultIcon") -eq ('"' + $exe.Replace('/', '\') + '",0')) 'project icon points to the branded executable'

    $shell = New-Object -ComObject WScript.Shell
    $shortcuts = @()
    foreach ($folder in @($start, $desktop)) {
        $path = Join-Path $folder "$product.lnk"
        Check (Test-Path -LiteralPath $path) 'requested shortcut exists'
        $shortcut = $shell.CreateShortcut($path)
        Check ($shortcut.TargetPath -eq $exe) 'shortcut targets installed UAV Studio executable'
        Check ($shortcut.IconLocation -eq "$exe,0") 'shortcut uses the dedicated application icon'
        Check ($shortcut.WorkingDirectory -eq (Split-Path -Parent $exe)) 'shortcut working directory is the application folder'
        $shortcuts += [ordered]@{path = $path; target = $shortcut.TargetPath; icon = $shortcut.IconLocation; description = $shortcut.Description}
    }
    $report.shortcuts = $shortcuts
    # Preserve an obsolete DLL, optional backend residue and a user's extra file.
    # Their contents are deliberately non-runnable; this is installer acceptance.
    New-Item -ItemType Directory -Path (Join-Path $install 'app/rviz_host') | Out-Null
    [IO.File]::WriteAllText((Join-Path $install 'app/obsolete-codec.dll'), 'Old codec fixture; never execute.')
    [IO.File]::WriteAllText((Join-Path $install 'app/rviz_host/old-host'), 'Old optional host fixture; never execute.')
    [IO.File]::WriteAllText((Join-Path $install 'app/user-notes.txt'), 'Extra user-file preservation fixture.')
    $oldAppPrefix = (Join-Path $install 'app') + [IO.Path]::DirectorySeparatorChar
    $oldUninstaller = Join-Path $install 'Uninstall.exe'
    $beforeFiles = @(Get-ChildItem -LiteralPath $install -File -Recurse | Where-Object {
        $_.FullName.StartsWith($oldAppPrefix, [StringComparison]::OrdinalIgnoreCase) -or $_.FullName -eq $oldUninstaller
    } | ForEach-Object {
        [ordered]@{path=[IO.Path]::GetRelativePath($install, $_.FullName); sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    } | Sort-Object { $_.path })
    $beforeCommand = ReadDefault "$registryRoot\$projectId\shell\open\command"
    $beforeRegistration = Get-ItemProperty -LiteralPath $uninstallRoot | Select-Object DisplayName,DisplayVersion,Publisher,Contact,URLInfoAbout,InstallId,InstallLocation,UninstallString | ConvertTo-Json -Compress
    RunInstaller '0' $install
    $transaction = Get-Content -LiteralPath (Join-Path $install 'last-install-transaction.json') -Raw | ConvertFrom-Json
    $afterFiles = @(Get-ChildItem -LiteralPath $transaction.backup -File -Recurse | ForEach-Object {
        [ordered]@{path=[IO.Path]::GetRelativePath($transaction.backup, $_.FullName); sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    } | Sort-Object { $_.path })
    Check (($beforeFiles | ConvertTo-Json -Compress) -eq ($afterFiles | ConvertTo-Json -Compress)) 'trusted same-path upgrade preserves every old payload, obsolete backend and extra user file byte-for-byte in backup'
    Check ((ReadDefault "$registryRoot\$projectId\shell\open\command") -eq $beforeCommand -and
        (Get-ItemProperty -LiteralPath $uninstallRoot | Select-Object DisplayName,DisplayVersion,Publisher,Contact,URLInfoAbout,InstallId,InstallLocation,UninstallString | ConvertTo-Json -Compress) -eq $beforeRegistration) 'same-path upgrade retains the correct associations and uninstall registration'
    Check (!(Test-Path -LiteralPath (Join-Path $desktop "$product.lnk"))) 'same-path desktop opt-out removes only the optional shortcut'
    Check (!(Test-Path -LiteralPath (Join-Path $install 'app/obsolete-codec.dll')) -and
        !(Test-Path -LiteralPath (Join-Path $install 'app/rviz_host')) -and
        !(Test-Path -LiteralPath (Join-Path $install 'app/user-notes.txt'))) 'fresh active payload does not merge old codecs, optional backend or user extras'
    $report.retained_backup_files = $afterFiles
    $report.automatic_in_place_upgrade_supported = $true
    $report.existing_install_retained = $true

    # Separate installations continue to protect each other's registrations.
    $secondInstall = Join-Path $ArtifactDirectory 'isolated-second-install'
    RunInstaller '0' $secondInstall
    $secondExe = Join-Path $secondInstall 'app/uav-studio.exe'
    Check ((Get-FileHash -LiteralPath $secondExe).Hash -eq (Get-FileHash -LiteralPath $ApplicationExecutable).Hash) 'separate fresh destination installs the selected payload after same-path upgrade'
    Check (!(Test-Path -LiteralPath (Join-Path $secondInstall 'app/obsolete-codec.dll')) -and
        !(Test-Path -LiteralPath (Join-Path $secondInstall 'app/rviz_host'))) 'fresh destination does not inherit old codec or optional backend residue'
    Check (!(Test-Path -LiteralPath (Join-Path $desktop "$product.lnk"))) 'desktop opt-out removes the previous optional shortcut'
    Check (Test-Path -LiteralPath (Join-Path $start "$product.lnk")) 'start-menu shortcut remains after desktop opt-out'

    $newerInstall = Join-Path $ArtifactDirectory 'isolated-newer-install'
    RunInstaller '1' $newerInstall
    $newerExe = Join-Path $newerInstall 'app/uav-studio.exe'
    Check (Test-Path -LiteralPath $newerExe) 'newer installation has its own application files'
    $uninstaller = Join-Path $secondInstall 'Uninstall.exe'
    $process = Start-Process -FilePath $uninstaller -ArgumentList '/S', "_?=$secondInstall" -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $ArtifactDirectory 'older-uninstall.out.log') -RedirectStandardError (Join-Path $ArtifactDirectory 'older-uninstall.err.log')
    Check ($process.ExitCode -eq 0 -and !(Test-Path -LiteralPath $secondExe)) 'older installation removes its application files'
    Check (Test-Path -LiteralPath $newerExe) 'older uninstall preserves the newer application files'
    $newerShortcut = $shell.CreateShortcut((Join-Path $start "$product.lnk"))
    Check ($newerShortcut.TargetPath -eq $newerExe) 'older uninstall preserves the newer shortcut'
    Check ((ReadDefault "$registryRoot\$projectId\shell\open\command") -eq ('"' + $newerExe.Replace('/', '\') + '" "%1"')) 'older uninstall preserves the newer file-open command'
    Check ((Get-ItemProperty -LiteralPath $uninstallRoot).InstallLocation -eq $newerInstall) 'older uninstall preserves the newer uninstall registration'
    $uninstaller = Join-Path $newerInstall 'Uninstall.exe'
    $process = Start-Process -FilePath $uninstaller -ArgumentList '/S', "_?=$newerInstall" -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $ArtifactDirectory 'newer-uninstall.out.log') -RedirectStandardError (Join-Path $ArtifactDirectory 'newer-uninstall.err.log')
    Check ($process.ExitCode -eq 0) 'isolated uninstallation succeeds'
    Check (!(Test-Path -LiteralPath $newerExe)) 'uninstallation removes application files'
    Check (!(Test-Path -LiteralPath (Join-Path $start "$product.lnk"))) 'uninstallation removes start-menu shortcut'
    Check (!(Test-Path -LiteralPath "$registryRoot\$projectId")) 'uninstallation removes the owned project registration'
    Check ($null -eq (ReadDefault "$registryRoot\.uavstudio")) 'uninstallation removes the owned extension default'
    Check (!(Test-Path -LiteralPath "$registryRoot\.uavstudio\OpenWithProgids") -or
        ((Get-Item -LiteralPath "$registryRoot\.uavstudio\OpenWithProgids").GetValueNames() -notcontains $projectId)) 'uninstallation removes its Open With entry'
    Check ((ReadDefault "$registryRoot\.slamfpv") -eq 'ExistingProjectOwner') 'uninstallation preserves the legacy extension owner'
    Check (!(Test-Path -LiteralPath $uninstallRoot)) 'uninstallation removes its own uninstall registration'
    Check (Test-Path -LiteralPath (Join-Path $transaction.backup 'app/user-notes.txt')) 'uninstalling separate destinations preserves the upgraded original backup and user extras'
    $report.success = $true
} catch {
    $report.error = $_.Exception.Message
    throw
} finally {
    # Only unique acceptance-owned keys are eligible for cleanup.
    if ($productId -notmatch '^UAVStudioBrandingAcceptance[0-9a-f]{12}$') { throw 'Unsafe acceptance registry namespace.' }
    $acceptanceKey = "HKCU:\Software\UAVStudioInstallerAcceptance\$runId"
    if (Test-Path -LiteralPath $acceptanceKey) { Remove-Item -LiteralPath $acceptanceKey -Recurse -Force }
    if (Test-Path -LiteralPath $uninstallRoot) { Remove-Item -LiteralPath $uninstallRoot -Recurse -Force }
    $report | ConvertTo-Json -Depth 7 | Set-Content -Encoding utf8 (Join-Path $ArtifactDirectory 'installer-check.json')
}
Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'installer-check.json')
