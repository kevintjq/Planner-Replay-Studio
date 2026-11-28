param(
    [Parameter(Mandatory=$true)][ValidateSet('Check','Prepare','Commit')][string]$Operation,
    [Parameter(Mandatory=$true)][string]$InstallRoot,
    [Parameter(Mandatory=$true)][string]$InstallId,
    [Parameter(Mandatory=$true)][string]$AppName,
    [Parameter(Mandatory=$true)][string]$ExecutableName,
    [Parameter(Mandatory=$true)][string]$UninstallKey,
    [Parameter(Mandatory=$true)][string]$AssociationRoot,
    [Parameter(Mandatory=$true)][string]$ManifestHash,
    [Parameter(Mandatory=$true)][string]$PayloadManifestFile,
    [Parameter(Mandatory=$true)][string]$StateFile
)

# Installer-only helper, compatible with inbox Windows PowerShell 5.1. The C++/Qt
# application neither imports this script nor needs a PowerShell process to run.
$ErrorActionPreference = 'Stop'
function Fail([string]$message,[int]$code=2) { $script:failureCode=$code; throw $message }
function FullPath([string]$path) { return [IO.Path]::GetFullPath($path).TrimEnd('\','/') }
function NoReparseAncestors([string]$path) {
    $item = [IO.DirectoryInfo]::new($path)
    while ($item) {
        if ($item.Exists -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Fail "Reparse installation path refused: $($item.FullName)" }
        $item=$item.Parent
    }
}
function SafeFiles([string]$directory) {
    NoReparseAncestors $directory
    $files=@(); $pending=[Collections.Generic.Stack[string]]::new(); $pending.Push($directory)
    while ($pending.Count) {
        foreach ($item in @(Get-ChildItem -LiteralPath $pending.Pop() -Force)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { Fail "Reparse payload entry refused: $($item.FullName)" }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) } else { $files += $item }
        }
    }
    return $files
}
function ReadJson([string]$path) { return [IO.File]::ReadAllText($path) | ConvertFrom-Json }
function Sha256([string]$path) {
    # Avoid dependence on the optional Get-FileHash module in a 32-bit installer
    # PowerShell host. SHA256 and file streams are inbox .NET Framework APIs.
    $hash=[Security.Cryptography.SHA256]::Create(); $stream=[IO.File]::OpenRead($path)
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','') }
    finally { $stream.Dispose(); $hash.Dispose() }
}
function IsPe([string]$path) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    $stream=[IO.File]::OpenRead($path)
    try {
        $reader=[IO.BinaryReader]::new($stream)
        if ($stream.Length -lt 128 -or $reader.ReadUInt16() -ne 0x5a4d) { return $false }
        $stream.Position=60; $offset=$reader.ReadUInt32()
        if ($offset -gt $stream.Length-6) { return $false }
        $stream.Position=$offset
        return $reader.ReadUInt32() -eq 0x4550 -and $reader.ReadUInt16() -eq 0x8664
    } finally { $stream.Dispose() }
}
function AssertTransactionPaths([bool]$oldInstallation) {
    if (!(Test-Path -LiteralPath $PayloadManifestFile -PathType Leaf) -or
        (Sha256 $PayloadManifestFile) -ine $ManifestHash) { Fail 'New payload manifest integrity check failed.' 4 }
    $manifest=ReadJson $PayloadManifestFile
    if ($manifest.schema_version -ne 1 -or $manifest.install_id -cne $InstallId -or $manifest.app_name -cne $AppName -or
        $manifest.executable_name -cne $ExecutableName) { Fail 'New payload identity is invalid.' 4 }
    # NSIS and inbox Windows PowerShell use legacy Win32 paths. Reserve the
    # longest transaction prefix, including a future whole-payload backup, before
    # extraction. Otherwise NSIS can silently omit resources at MAX_PATH.
    $backup=Join-Path $root ('previous-install-'+('0'*32))
    $paths=@('uav-studio-install.json')+@($manifest.files | ForEach-Object { $_.path })
    foreach ($relative in $paths) {
        if (!$relative -or $relative -match '(^/|\\|:|(^|/)\.\.?(/|$))') { Fail 'Unsafe manifest path.' 4 }
        if ((Join-Path (Join-Path $backup 'app') $relative).Length -ge 260) {
            Fail 'Installation path is too long for safe extraction and upgrade. Choose a shorter local folder; existing files were preserved.'
        }
    }
    if ($oldInstallation) {
        foreach ($file in @(SafeFiles (Join-Path $root 'app'))) {
            $relative=$file.FullName.Substring((Join-Path $root 'app').Length+1)
            if ((Join-Path (Join-Path $backup 'app') $relative).Length -ge 260) {
                Fail 'Existing payload paths are too long for a safe backup. Choose a shorter installation folder; existing files were preserved.'
            }
        }
    }
}
function RegistryValues([string]$path) {
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key=$base.OpenSubKey($path)
        if (!$key) { return @{} }
        try { $values=@{}; foreach ($name in $key.GetValueNames()) { $values[$name]=$key.GetValue($name) }; return $values }
        finally { $key.Dispose() }
    } finally { $base.Dispose() }
}
function AssertOldInstallation {
    if (Test-Path -LiteralPath $root -PathType Leaf) { Fail 'Installation destination is a file.' }
    NoReparseAncestors $root
    if (!(Test-Path -LiteralPath $root)) { return $false }
    $entries=@(Get-ChildItem -LiteralPath $root -Force)
    if (!$entries.Count) { return $false }
    $app=Join-Path $root 'app'; $uninstaller=Join-Path $root 'Uninstall.exe'
    if (!(Test-Path -LiteralPath $app -PathType Container) -or !(Test-Path -LiteralPath $uninstaller -PathType Leaf)) {
        Fail 'Occupied destination is not a verified installation; no files were changed.'
    }
    if ((Get-Item -LiteralPath $uninstaller -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        Fail 'Reparse uninstaller refused; existing files were preserved.'
    }
    $oldFiles=@(SafeFiles $app)
    $registration=RegistryValues $UninstallKey
    if (!$registration.InstallLocation -or (FullPath $registration.InstallLocation) -ine $root -or
        $registration.DisplayName -cne $AppName -or $registration.UninstallString -cne ('"'+$uninstaller+'"') -or
        ($registration.InstallId -and $registration.InstallId -cne $InstallId)) {
        Fail 'Existing installation registration/identity does not match this exact product and path.'
    }
    $exe=Join-Path $app $ExecutableName
    if (!(IsPe $exe)) { Fail 'Existing product executable is not the expected x64 PE program.' }
    $manifestPath=Join-Path $app 'uav-studio-install.json'
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $old=ReadJson $manifestPath
        if ($old.schema_version -ne 1 -or $old.install_id -cne $InstallId -or $old.app_name -cne $AppName -or
            $old.executable_name -cne $ExecutableName) { Fail 'Existing payload manifest belongs to a different product.' }
        $entry=@($old.files | Where-Object path -CEQ $ExecutableName)
        if ($entry.Count -ne 1 -or (Sha256 $exe) -ine $entry[0].sha256) {
            Fail 'Existing executable differs from its ownership manifest; preserving it without replacement.'
        }
    } else {
        # Older previews had no install manifest. Registry alone is insufficient:
        # require the exact owned open command and the two historical package
        # declarations alongside both x64 product executables.
        $command=RegistryValues ($AssociationRoot+'\'+$InstallId+'.Project\shell\open\command')
        $backendPath=Join-Path $app 'native-rviz-backend.json'; $runtimePath=Join-Path $app 'windows-runtime.json'
        if ($command[''] -cne ('"'+$exe+'" "%1"') -or $registration.DisplayVersion -notmatch '^0\.1\.\d+' -or
            !(IsPe (Join-Path $app 'uav-studio-cli.exe')) -or !(Test-Path -LiteralPath $backendPath -PathType Leaf) -or
            !(Test-Path -LiteralPath $runtimePath -PathType Leaf)) { Fail 'Legacy installation lacks sufficient product ownership evidence.' }
        $backend=ReadJson $backendPath; $runtime=ReadJson $runtimePath
        if ($backend.package_profile -notin @('Standalone','OptionalNativeRviz') -or $runtime.profile -cne $backend.package_profile -or
            $backend.standalone_requires_ros -ne $false -or $backend.standalone_requires_wsl -ne $false -or
            $backend.standalone_requires_python -ne $false -or $runtime.python_runtime_included -ne $false -or
            $runtime.ros_windows_runtime_included -ne $false) { Fail 'Legacy runtime declarations do not identify the expected Windows product.' }
    }
    # A running/mapped executable or a sharing lock must fail before renaming any
    # old content. ReadWrite + no sharing tests access without writing a byte.
    foreach ($file in @($oldFiles)+@(Get-Item -LiteralPath $uninstaller)) {
        try { $lock=[IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None); $lock.Dispose() }
        catch { Fail "Application files are in use or read-only; close UAV Studio and retry. Preserved: $($file.FullName)" 3 }
    }
    return $true
}
function AssertStagedPayload([string]$directory) {
    $manifestPath=Join-Path $directory 'uav-studio-install.json'
    if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
        (Sha256 $manifestPath) -ine $ManifestHash) { Fail 'New payload manifest integrity check failed.' 4 }
    $manifest=ReadJson $manifestPath
    if ($manifest.schema_version -ne 1 -or $manifest.install_id -cne $InstallId -or $manifest.app_name -cne $AppName -or
        $manifest.executable_name -cne $ExecutableName -or $manifest.version -notmatch '^\d+\.\d+\.\d+$') { Fail 'New payload identity is invalid.' 4 }
    $expected=@{}
    foreach ($file in @($manifest.files)) {
        if (!$file.path -or $file.path -match '(^/|\\|:|(^|/)\.\.?(/|$))' -or $expected.ContainsKey($file.path)) { Fail 'Unsafe or duplicate manifest path.' 4 }
        $expected[$file.path]=$file
    }
    $seen=@{}
    foreach ($file in @(SafeFiles $directory)) {
        $relative=$file.FullName.Substring($directory.Length+1).Replace('\','/')
        if ($relative -ceq 'uav-studio-install.json') { continue }
        if (!$expected.ContainsKey($relative) -or $file.Length -ne $expected[$relative].bytes -or
            (Sha256 $file.FullName) -ine $expected[$relative].sha256) { Fail "New payload file failed integrity: $relative" 4 }
        $seen[$relative]=$true
    }
    if ($seen.Count -ne $expected.Count -or !(IsPe (Join-Path $directory $ExecutableName))) { Fail 'New payload is incomplete.' 4 }
    return $manifest
}

$script:failureCode=5
try {
    if ($InstallId -notmatch '^[A-Za-z0-9_.-]+$' -or $ExecutableName -notmatch '^[A-Za-z0-9_.-]+\.exe$' -or
        $ManifestHash -notmatch '^[0-9a-fA-F]{64}$' -or $UninstallKey -cne ('Software\Microsoft\Windows\CurrentVersion\Uninstall\'+$InstallId)) { Fail 'Invalid installer identity.' }
    $root=FullPath $InstallRoot
    if ($root -notmatch '^[A-Za-z]:\\.+' -or $root -eq [IO.Path]::GetPathRoot($root).TrimEnd('\')) { Fail 'Choose a local application subdirectory.' }
    if ($Operation -eq 'Check') { $old=AssertOldInstallation; AssertTransactionPaths $old; Write-Output 'Installation destination verified.'; exit 0 }
    if ($Operation -eq 'Prepare') {
        $old=AssertOldInstallation
        AssertTransactionPaths $old
        $suffix=[Guid]::NewGuid().ToString('N')
        $state=[ordered]@{schema_version=1;install_id=$InstallId;root=$root;old_install=[bool]$old;
            staging=(Join-Path $root ('.uav-staging-'+$suffix));backup=(Join-Path $root ('previous-install-'+$suffix))}
        [IO.File]::WriteAllText($StateFile,($state | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText(($StateFile+'.ini'),("[Install]`r`nStaging="+$state.staging+"`r`n"),[Text.UnicodeEncoding]::new($false,$true))
        Write-Output 'Fresh same-volume staging planned.'; exit 0
    }
    $state=ReadJson $StateFile
    if ($state.schema_version -ne 1 -or $state.install_id -cne $InstallId -or $state.root -ine $root -or
        $state.staging -notmatch ('^'+[regex]::Escape($root)+'\\\.uav-staging-[0-9a-f]{32}$') -or
        $state.backup -notmatch ('^'+[regex]::Escape($root)+'\\previous-install-[0-9a-f]{32}$')) { Fail 'Invalid staging transaction.' }
    $newApp=Join-Path $state.staging 'app'; $newUninstaller=Join-Path $state.staging 'Uninstall.exe'
    $manifest=AssertStagedPayload $newApp
    if (!(Test-Path -LiteralPath $newUninstaller -PathType Leaf) -or
        (Get-Item -LiteralPath $newUninstaller -Force).Length -eq 0 -or
        ((Get-Item -LiteralPath $newUninstaller -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Fail 'New uninstaller was not completely staged as a regular file.' 4
    }
    # Ignore only our own unpredictable staging directory on a fresh target.
    if (!$state.old_install) {
        NoReparseAncestors $root
        if (@(Get-ChildItem -LiteralPath $root -Force | Where-Object FullName -INE $state.staging).Count) { Fail 'Fresh destination changed during extraction.' }
    } elseif (!(AssertOldInstallation)) { Fail 'Previous installation changed during extraction.' }
    NoReparseAncestors $state.backup
    if (Test-Path -LiteralPath $state.backup) { Fail 'Recovery destination already exists; no previous files were replaced.' }
    $app=Join-Path $root 'app'; $uninstaller=Join-Path $root 'Uninstall.exe'; $moved=@()
    try {
        if ($state.old_install) {
            [IO.Directory]::CreateDirectory($state.backup) | Out-Null
            [IO.Directory]::Move($app,(Join-Path $state.backup 'app')); $moved+=,@($app,(Join-Path $state.backup 'app'),'directory')
            [IO.File]::Move($uninstaller,(Join-Path $state.backup 'Uninstall.exe')); $moved+=,@($uninstaller,(Join-Path $state.backup 'Uninstall.exe'),'file')
        }
        [IO.Directory]::Move($newApp,$app); $moved+=,@($newApp,$app,'directory')
        [IO.File]::Move($newUninstaller,$uninstaller); $moved+=,@($newUninstaller,$uninstaller,'file')
    } catch {
        $errorMessage=$_.Exception.Message
        for ($i=$moved.Count-1;$i -ge 0;--$i) {
            try {
                if ($moved[$i][2] -eq 'directory') { [IO.Directory]::Move($moved[$i][1],$moved[$i][0]) }
                else { [IO.File]::Move($moved[$i][1],$moved[$i][0]) }
            } catch { $errorMessage+='; preserved recovery content at '+$moved[$i][1] }
        }
        Fail ('Directory replacement failed; previous content retained. '+$errorMessage) 5
    }
    # Preserve all old extra files and codecs in the backup. Never merge them into
    # the active app, and never traverse/delete a user's project or settings path.
    try {
        $journal=Join-Path $root 'last-install-transaction.json'
        if ((Test-Path -LiteralPath $journal) -and ((Get-Item -LiteralPath $journal -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'The optional transaction report is a reparse point and was left unchanged.'
        }
        [IO.File]::WriteAllText($journal,($state | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    }
    catch { Write-Output ('Installation completed; could not write the optional transaction report: '+$_.Exception.Message) }
    Write-Output ('Installed '+$manifest.version+'; previous content retained at '+$state.backup)
    exit 0
} catch { Write-Output $_.Exception.Message; exit $script:failureCode }
