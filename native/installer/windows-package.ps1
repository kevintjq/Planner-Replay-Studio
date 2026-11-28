function Assert-FreshWindowsStaging([string]$AppDirectory, [string]$FFmpegDirectory) {
    # CMake's Windows install contains the two programs and linked FFmpeg DLLs only.
    # Deployment adds Qt/OpenCV/CRT/notices later. Reusing that output can retain an
    # obsolete plugin, codec ABI, test program or private media in NSIS File /r.
    $AppDirectory = (Resolve-Path -LiteralPath $AppDirectory).Path
    $FFmpegDirectory = (Resolve-Path -LiteralPath $FFmpegDirectory).Path
    $expected = @{}
    foreach ($name in @('uav-studio.exe', 'uav-studio-cli.exe')) { $expected[$name] = $null }
    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $FFmpegDirectory 'bin') -File -Filter '*.dll')) {
        $expected[$file.Name] = $file.FullName
    }
    $unexpected = @()
    $mismatched = @()
    foreach ($item in @(Get-ChildItem -LiteralPath $AppDirectory -Force)) {
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or !$expected.ContainsKey($item.Name)) {
            $unexpected += $item.Name
        } elseif ($expected[$item.Name] -and
            (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $expected[$item.Name] -Algorithm SHA256).Hash) {
            $mismatched += $item.Name
        }
    }
    if ($unexpected.Count -or $mismatched.Count) {
        throw "Packaging requires fresh CMake install staging; no files were removed. Unexpected entries: $($unexpected -join ', '); FFmpeg DLLs differing from the selected development package: $($mismatched -join ', '). Install into a new directory before packaging."
    }
    foreach ($name in @('uav-studio.exe', 'uav-studio-cli.exe')) {
        if (!(Test-Path -LiteralPath (Join-Path $AppDirectory $name) -PathType Leaf)) { throw "Fresh staging is missing $name." }
    }
    return [ordered]@{
        input_profile = 'FreshCMakeWindowsInstall'
        allowed_files = @($expected.Keys | Sort-Object)
        checked_existing_files = @(Get-ChildItem -LiteralPath $AppDirectory -File).Count
        stale_entries = @(); ffmpeg_mismatches = @(); files_removed = 0
    }
}

function Write-WindowsInstallManifest([string]$AppDirectory, [string]$InstallId, [string]$Version,
    [string]$AppName = 'UAV Studio', [string]$ExecutableName = 'uav-studio.exe', [string]$OutputFile) {
    if ($InstallId -notmatch '^[A-Za-z0-9_.-]+$' -or $Version -notmatch '^\d+\.\d+\.\d+$' -or
        $ExecutableName -notmatch '^[A-Za-z0-9_.-]+\.exe$') { throw 'Invalid install manifest identity or version.' }
    $AppDirectory = (Resolve-Path -LiteralPath $AppDirectory).Path
    $ancestor = [IO.DirectoryInfo]::new($AppDirectory)
    while ($ancestor) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Install payload root cannot traverse a reparse point: $($ancestor.FullName)" }
        $ancestor = $ancestor.Parent
    }
    $manifestName = 'uav-studio-install.json'
    $files = @(); $pending = [Collections.Generic.Stack[string]]::new(); $pending.Push($AppDirectory)
    while ($pending.Count) {
        foreach ($item in @(Get-ChildItem -LiteralPath $pending.Pop() -Force)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Install payload cannot contain reparse points: $($item.FullName)" }
            if ($item.PSIsContainer) { $pending.Push($item.FullName); continue }
            $relative = $item.FullName.Substring($AppDirectory.Length + 1).Replace('\','/')
            if ($relative -eq $manifestName) { continue }
            $files += [ordered]@{path=$relative; bytes=$item.Length; sha256=(Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash}
        }
    }
    if ($ExecutableName -notin $files.path) { throw 'Install manifest is missing the application executable.' }
    $manifest = [ordered]@{schema_version=1; install_id=$InstallId; app_name=$AppName; version=$Version;
        executable_name=$ExecutableName; architecture='x64'; files=@($files | Sort-Object path)}
    $path = if ($OutputFile) { [IO.Path]::GetFullPath($OutputFile) } else { Join-Path $AppDirectory $manifestName }
    if ($OutputFile -and $path.StartsWith($AppDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'An external install manifest must be outside the original payload to preserve its identity.'
    }
    [IO.File]::WriteAllText($path,($manifest | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
    return [ordered]@{path=$path;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;files=$files.Count;install_id=$InstallId;version=$Version}
}
