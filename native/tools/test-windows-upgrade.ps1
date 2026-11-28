param(
    [Parameter(Mandatory=$true)][string]$NsisCompiler,
    [Parameter(Mandatory=$true)][string]$ApplicationExecutable,
    [string]$ArtifactDirectory
)

$ErrorActionPreference='Stop'
$repository=(Resolve-Path "$PSScriptRoot/../..").Path
. "$repository/native/installer/windows-package.ps1"
$NsisCompiler=(Resolve-Path -LiteralPath $NsisCompiler).Path
$ApplicationExecutable=(Resolve-Path -LiteralPath $ApplicationExecutable).Path
$runId=[Guid]::NewGuid().ToString('N').Substring(0,12)
if (!$ArtifactDirectory) { $ArtifactDirectory=Join-Path $repository "captures/windows-upgrade-$runId" }
$ArtifactDirectory=[IO.Path]::GetFullPath($ArtifactDirectory)
if (!$ArtifactDirectory.StartsWith($repository+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Upgrade artifacts must remain inside this repository.' }
if (Test-Path -LiteralPath $ArtifactDirectory) { throw 'Choose a fresh artifact directory.' }
New-Item -ItemType Directory -Path $ArtifactDirectory | Out-Null
$metadata=Get-Content -LiteralPath (Join-Path $repository 'app-metadata.json') -Raw | ConvertFrom-Json
$product="UAV Studio Upgrade Acceptance $runId"
$productId="UAVStudioUpgradeAcceptance$runId"
$associationRoot="Software\UAVStudioUpgradeAcceptance\$runId\Classes"
$registryRoot="HKCU:\$associationRoot"
$uninstallKey="Software\Microsoft\Windows\CurrentVersion\Uninstall\$productId"
$uninstallRoot="HKCU:\$uninstallKey"
$start=Join-Path $ArtifactDirectory 'shortcuts/start'
$desktop=Join-Path $ArtifactDirectory 'shortcuts/desktop'
$powerShell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$helper=Join-Path $repository 'native/installer/windows-upgrade.ps1'
$report=[ordered]@{kind='isolated-in-place-upgrade';production_registry_touched=$false;application_executed=$false;
    runnable_distribution_verified=$false;inbox_powershell=$powerShell;checks=@();success=$false}
$script:run=0
function Check([bool]$condition,[string]$name) {
    $script:report.checks += [ordered]@{name=$name;passed=$condition}
    if (!$condition) { throw "Upgrade acceptance failed: $name" }
}
function Snapshot([string]$directory) {
    return @(@(Get-ChildItem -LiteralPath $directory -File -Force -Recurse) | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($directory.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    } | Sort-Object { $_.path })
}
function SameSnapshot($left,$right) { return ($left | ConvertTo-Json -Depth 5 -Compress) -ceq ($right | ConvertTo-Json -Depth 5 -Compress) }
function RunProcess([string]$program,[string[]]$arguments,[string]$name,[int]$expected=0) {
    $script:run++
    $quoted=@($arguments | ForEach-Object { '"'+$_.Replace('"','\"')+'"' })
    $process=Start-Process -FilePath $program -ArgumentList $quoted -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $ArtifactDirectory "$script:run-$name.out.log") `
        -RedirectStandardError (Join-Path $ArtifactDirectory "$script:run-$name.err.log")
    Check ($process.ExitCode -eq $expected) "$name returns $expected (actual $($process.ExitCode))"
}
function Install([string]$setup,[string]$destination,[string]$name,[int]$expected=0) {
    $script:run++
    # NSIS parses /S and /D itself: quote neither switch, and keep /D last so
    # destinations with spaces are accepted by its documented command grammar.
    $process=Start-Process -FilePath $setup -ArgumentList '/S','/DESKTOPSHORTCUT=1',"/D=$destination" -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $ArtifactDirectory "$script:run-$name.out.log") `
        -RedirectStandardError (Join-Path $ArtifactDirectory "$script:run-$name.err.log")
    if (!$process.WaitForExit(45000)) {
        $process.Kill(); $process.WaitForExit(); throw "Isolated $name exceeded 45 seconds."
    }
    $process.WaitForExit()
    Check ($process.ExitCode -eq $expected) "$name returns $expected (actual $($process.ExitCode))"
}
function NewPayload([string]$version,[bool]$old) {
    $directory=Join-Path $ArtifactDirectory "payload-$version"
    New-Item -ItemType Directory -Path $directory | Out-Null
    foreach ($name in @('uav-studio.exe','uav-studio-cli.exe')) {
        Copy-Item -LiteralPath $ApplicationExecutable -Destination (Join-Path $directory $name)
        if ($old) { [IO.File]::AppendAllText((Join-Path $directory $name),'Old non-executed installer fixture marker.') }
    }
    [IO.File]::WriteAllText((Join-Path $directory 'codec-current.dll'),"Non-runnable codec marker for $version")
    [IO.File]::WriteAllText((Join-Path $directory 'README.txt'),'Installer fixture only. Product executables are never launched; dependencies are intentionally absent.')
    [IO.File]::WriteAllText((Join-Path $directory 'native-rviz-backend.json'),'{"package_profile":"Standalone","standalone_requires_ros":false,"standalone_requires_wsl":false,"standalone_requires_python":false}')
    [IO.File]::WriteAllText((Join-Path $directory 'windows-runtime.json'),'{"profile":"Standalone","python_runtime_included":false,"ros_windows_runtime_included":false}')
    $manifest=Write-WindowsInstallManifest $directory $productId $version $product
    $setup=Join-Path $ArtifactDirectory "installer-$version.exe"
    RunProcess $NsisCompiler @('/WX',"/DAPP_DIR=$directory","/DSETUP_FILE=$setup","/DVERSION=$version",
        "/DPRODUCT=$product","/DPRODUCT_ID=$productId","/DAPP_EXE=$($metadata.executable_name).exe","/DASSOCIATION_ROOT=$associationRoot",
        "/DSTARTMENU_DIRECTORY=$start","/DDESKTOP_DIRECTORY=$desktop","/DINSTALL_MANIFEST_SHA256=$($manifest.sha256)",
        "/DDIAGNOSTIC_LOG=$(Join-Path $ArtifactDirectory 'installer-helper.log')",
        "/DAUTHOR=$($metadata.author)","/DPROJECT_URL=$($metadata.github_url)","/DCONTACT_EMAIL=$($metadata.email)",
        (Join-Path $repository 'native/installer/windows.nsi')) "compile-$version"
    return [pscustomobject]@{directory=$directory;setup=$setup;manifest=$manifest}
}
function RunHelper([string]$operation,[string]$root,[string]$state,[string]$name,[int]$expected=0) {
    RunProcess $powerShell @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',$helper,
        '-Operation',$operation,'-InstallRoot',$root,'-InstallId',$productId,'-AppName',$product,
        '-ExecutableName','uav-studio.exe','-UninstallKey',$uninstallKey,'-AssociationRoot',$associationRoot,
        '-ManifestHash',$script:new.manifest.sha256,'-PayloadManifestFile',$script:new.manifest.path,'-StateFile',$state) $name $expected
}
function PrepareStaged([string]$root,[string]$name) {
    $state=Join-Path $ArtifactDirectory "$name-state.json"
    RunHelper 'Prepare' $root $state "$name-prepare"
    $transaction=Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
    New-Item -ItemType Directory -Path $transaction.staging | Out-Null
    Copy-Item -LiteralPath $script:new.directory -Destination (Join-Path $transaction.staging 'app') -Recurse
    Copy-Item -LiteralPath (Join-Path $root 'Uninstall.exe') -Destination (Join-Path $transaction.staging 'Uninstall.exe')
    return [pscustomobject]@{file=$state;transaction=$transaction}
}
try {
    $old=NewPayload '0.1.0' $true
    $new=NewPayload '0.1.1' $false
    $root=Join-Path $ArtifactDirectory 'upgrade-install'
    Install $old.setup $root 'old-version-install'
    New-Item -ItemType Directory -Path (Join-Path $root 'app/rviz_host') | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'app/obsolete-codec.dll'),'Old codec; must not remain active.')
    [IO.File]::WriteAllText((Join-Path $root 'app/rviz_host/old-host'),'Old optional backend; preserve backup only.')
    [IO.File]::WriteAllText((Join-Path $root 'app/user-notes.txt'),'User-owned extra file inside old app.')
    [IO.File]::WriteAllText((Join-Path $root 'project.uavstudio'),'User project sentinel; no actual source media.')
    [IO.File]::WriteAllText((Join-Path $root 'settings.json'),'User settings sentinel.')
    $oldApp=Snapshot (Join-Path $root 'app')
    $oldUninstallerHash=(Get-FileHash -LiteralPath (Join-Path $root 'Uninstall.exe')).Hash
    Install $new.setup $root 'same-path-version-upgrade'
    $transaction=Get-Content -LiteralPath (Join-Path $root 'last-install-transaction.json') -Raw | ConvertFrom-Json
    Check (SameSnapshot $oldApp (Snapshot (Join-Path $transaction.backup 'app'))) 'whole old app including codecs, optional backend and user extras is retained byte-for-byte'
    Check ((Get-FileHash -LiteralPath (Join-Path $transaction.backup 'Uninstall.exe')).Hash -eq $oldUninstallerHash) 'old uninstaller is retained byte-for-byte'
    Check (SameSnapshot (Snapshot $new.directory) (Snapshot (Join-Path $root 'app'))) 'active app exactly matches the complete new payload, with no stale codec/plugin merge'
    Check ((Get-FileHash -LiteralPath (Join-Path $root 'app/uav-studio.exe')).Hash -ne $oldApp.Where({$_.path -eq 'uav-studio.exe'})[0].sha256) 'new application bytes replace old version bytes'
    Check ((Get-ItemProperty -LiteralPath $uninstallRoot).DisplayVersion -eq '0.1.1') 'same product registration reports the new version'
    Check ((Get-Content -LiteralPath (Join-Path $root 'project.uavstudio') -Raw) -eq 'User project sentinel; no actual source media.' -and
        (Get-Content -LiteralPath (Join-Path $root 'settings.json') -Raw) -eq 'User settings sentinel.') 'project and settings sentinels outside app remain unchanged'
    $report.successful_transaction=$transaction

    $arbitrary=Join-Path $ArtifactDirectory 'arbitrary-occupied-folder'
    New-Item -ItemType Directory -Path (Join-Path $arbitrary 'app') | Out-Null
    [IO.File]::WriteAllText((Join-Path $arbitrary 'app/user.txt'),'Arbitrary occupied application directory.')
    $before=Snapshot $arbitrary
    Install $new.setup $arbitrary 'arbitrary-root-refused' 2
    Check (SameSnapshot $before (Snapshot $arbitrary)) 'arbitrary occupied folder is refused without byte changes'

    # Derive an absolute length: a short artifact folder must not accidentally
    # turn this rejection case into a valid destination. A 210-character root
    # plus the reserved whole-payload backup exceeds MAX_PATH for the manifest.
    $longPrefix=Join-Path $ArtifactDirectory 'long-root-'
    $tooLong=$longPrefix+('x'*[Math]::Max(1,210-$longPrefix.Length))
    Install $new.setup $tooLong 'long-destination-refused-before-extraction' 2
    Check (!(Test-Path -LiteralPath $tooLong)) 'overlong destination is refused before creating any staged payload'

    $longExtra=Join-Path $ArtifactDirectory 'long-old-extra'
    Install $old.setup $longExtra 'long-old-extra-baseline'
    $extraPrefix=Join-Path $longExtra 'app/'
    $extraPath=Join-Path $extraPrefix (('x'*(230-$extraPrefix.Length))+'.txt')
    [IO.File]::WriteAllText($extraPath,'User-owned long filename; safe backup would exceed MAX_PATH.')
    $before=Snapshot $longExtra
    Install $new.setup $longExtra 'long-existing-backup-refused' 2
    Check (SameSnapshot $before (Snapshot $longExtra)) 'overlong backup of existing extra files is refused without byte changes'

    $wrong=Join-Path $ArtifactDirectory 'wrong-identity'
    Install $old.setup $wrong 'wrong-identity-baseline'
    $manifestPath=Join-Path $wrong 'app/uav-studio-install.json'
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.install_id='DifferentProduct'
    [IO.File]::WriteAllText($manifestPath,($manifest | ConvertTo-Json -Depth 6))
    $before=Snapshot $wrong
    Install $new.setup $wrong 'wrong-manifest-product-refused' 2
    Check (SameSnapshot $before (Snapshot $wrong)) 'wrong manifest product is refused without byte changes'

    $locked=Join-Path $ArtifactDirectory 'locked-old-app'
    Install $old.setup $locked 'locked-old-baseline'
    $before=Snapshot $locked
    $lock=[IO.File]::Open((Join-Path $locked 'app/uav-studio.exe'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try { Install $new.setup $locked 'locked-application-refused' 3 } finally { $lock.Dispose() }
    Check (SameSnapshot $before (Snapshot $locked)) 'locked old application remains intact; installer does not force-kill it'

    $legacy=Join-Path $ArtifactDirectory 'verified-legacy'
    Install $old.setup $legacy 'legacy-baseline'
    Remove-Item -LiteralPath (Join-Path $legacy 'app/uav-studio-install.json')
    Remove-ItemProperty -LiteralPath $uninstallRoot -Name InstallId
    $beforeLegacy=Snapshot (Join-Path $legacy 'app')
    Install $new.setup $legacy 'legacy-no-manifest-trusted-upgrade'
    $legacyTransaction=Get-Content -LiteralPath (Join-Path $legacy 'last-install-transaction.json') -Raw | ConvertFrom-Json
    Check (SameSnapshot $beforeLegacy (Snapshot (Join-Path $legacyTransaction.backup 'app'))) 'legacy recognition requires registry, exact path, association and historical runtime identifiers; old app preserved'
    $insufficient=Join-Path $ArtifactDirectory 'insufficient-legacy'
    Install $old.setup $insufficient 'insufficient-legacy-baseline'
    Remove-Item -LiteralPath (Join-Path $insufficient 'app/uav-studio-install.json')
    Remove-Item -LiteralPath (Join-Path $insufficient 'app/windows-runtime.json')
    $before=Snapshot $insufficient
    Install $new.setup $insufficient 'insufficient-legacy-refused' 2
    Check (SameSnapshot $before (Snapshot $insufficient)) 'registry-only legacy evidence is insufficient and all bytes remain unchanged'

    $corrupt=Join-Path $ArtifactDirectory 'corrupt-staging'
    Install $old.setup $corrupt 'corrupt-stage-baseline'
    $beforeApp=Snapshot (Join-Path $corrupt 'app')
    $beforeUninstaller=(Get-FileHash -LiteralPath (Join-Path $corrupt 'Uninstall.exe')).Hash
    $stage=PrepareStaged $corrupt 'corrupt-stage'
    [IO.File]::AppendAllText((Join-Path $stage.transaction.staging 'app/codec-current.dll'),'tampered')
    RunHelper 'Commit' $corrupt $stage.file 'corrupt-payload-refused' 4
    Check (SameSnapshot $beforeApp (Snapshot (Join-Path $corrupt 'app'))) 'corrupted staged payload never replaces old app'
    Check ((Get-FileHash -LiteralPath (Join-Path $corrupt 'Uninstall.exe')).Hash -eq $beforeUninstaller) 'corrupted staged payload preserves old uninstaller'

    $rollback=Join-Path $ArtifactDirectory 'replacement-failure'
    Install $old.setup $rollback 'replacement-failure-baseline'
    $beforeApp=Snapshot (Join-Path $rollback 'app')
    $beforeUninstaller=(Get-FileHash -LiteralPath (Join-Path $rollback 'Uninstall.exe')).Hash
    $stage=PrepareStaged $rollback 'replacement-failure'
    $lock=[IO.File]::Open((Join-Path $stage.transaction.staging 'Uninstall.exe'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try { RunHelper 'Commit' $rollback $stage.file 'replacement-failure-rollback' 5 } finally { $lock.Dispose() }
    Check (SameSnapshot $beforeApp (Snapshot (Join-Path $rollback 'app'))) 'replacement failure after directory moves restores old app bytes'
    Check ((Get-FileHash -LiteralPath (Join-Path $rollback 'Uninstall.exe')).Hash -eq $beforeUninstaller) 'replacement failure restores old uninstaller bytes'
    Check (SameSnapshot (Snapshot $new.directory) (Snapshot (Join-Path $stage.transaction.staging 'app'))) 'replacement failure retains complete fresh staged payload for diagnosis'
    # The complete-package runtime fixture supplies its own manifest from outside
    # production staging. Distinct JSON whitespace proves NSIS actually embeds
    # that overlay instead of copying the source manifest by accident.
    $sourceManifestHash=(Get-FileHash -LiteralPath $new.manifest.path).Hash
    $externalPath=Join-Path $ArtifactDirectory 'external-install-manifest.json'
    [IO.File]::WriteAllText($externalPath,([IO.File]::ReadAllText($new.manifest.path)+"`r`n"),[Text.UTF8Encoding]::new($false))
    $externalHash=(Get-FileHash -LiteralPath $externalPath).Hash
    $externalSetup=Join-Path $ArtifactDirectory 'installer-external-manifest.exe'
    RunProcess $NsisCompiler @('/WX',"/DAPP_DIR=$($new.directory)","/DSETUP_FILE=$externalSetup",'/DVERSION=0.1.1',
        "/DPRODUCT=$product","/DPRODUCT_ID=$productId","/DAPP_EXE=$($metadata.executable_name).exe","/DASSOCIATION_ROOT=$associationRoot",
        "/DSTARTMENU_DIRECTORY=$start","/DDESKTOP_DIRECTORY=$desktop","/DINSTALL_MANIFEST_SHA256=$externalHash",
        "/DINSTALL_MANIFEST_FILE=$externalPath","/DAUTHOR=$($metadata.author)","/DPROJECT_URL=$($metadata.github_url)","/DCONTACT_EMAIL=$($metadata.email)",
        (Join-Path $repository 'native/installer/windows.nsi')) 'compile-external-manifest'
    $externalRoot=Join-Path $ArtifactDirectory 'external-manifest-install'
    Install $externalSetup $externalRoot 'external-manifest-install'
    Check ($externalHash -ne $sourceManifestHash -and
        (Get-FileHash -LiteralPath (Join-Path $externalRoot 'app/uav-studio-install.json')).Hash -eq $externalHash -and
        (Get-FileHash -LiteralPath $new.manifest.path).Hash -eq $sourceManifestHash) 'external manifest is installed exactly while production staging ownership document stays unchanged'
    $report.limitations=@('Installer fixtures intentionally omit dependencies and never execute the application.',
        'Legacy fixture exercises documented historical identifiers, not every historical published binary.',
        'Controlled sharing locks and replacement failure are covered; abrupt power loss and adversarial same-user races are not.',
        'Best-effort synchronous rename rollback is covered; general automatic crash recovery remains deferred.')
    $report.success=$true
} catch { $report.error=$_.Exception.Message; throw }
finally {
    if ($productId -notmatch '^UAVStudioUpgradeAcceptance[0-9a-f]{12}$') { throw 'Unsafe acceptance namespace.' }
    $ownedRegistry="HKCU:\Software\UAVStudioUpgradeAcceptance\$runId"
    try {
        if (Test-Path -LiteralPath $ownedRegistry) { Remove-Item -LiteralPath $ownedRegistry -Recurse -Force }
        if (Test-Path -LiteralPath $uninstallRoot) { Remove-Item -LiteralPath $uninstallRoot -Recurse -Force }
    } catch { $report.registry_cleanup_error=$_.Exception.Message }
    $report | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $ArtifactDirectory 'report.json')
}
Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'report.json')
