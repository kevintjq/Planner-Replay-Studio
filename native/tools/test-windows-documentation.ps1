param([string]$ArtifactDirectory)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
. "$repository/native/installer/windows-documentation.ps1"
if (!$ArtifactDirectory) { $ArtifactDirectory = Join-Path $repository ("captures/windows-documentation-" + [Guid]::NewGuid().ToString('N')) }
$ArtifactDirectory = [IO.Path]::GetFullPath($ArtifactDirectory)
if (!$ArtifactDirectory.StartsWith($repository + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Documentation regression artifacts must remain inside this repository.'
}
if (Test-Path -LiteralPath $ArtifactDirectory) { throw 'Choose a fresh artifact directory.' }
New-Item -ItemType Directory -Path $ArtifactDirectory | Out-Null
$report = [ordered]@{kind='windows-offline-documentation'; checks=@(); application_executed=$false; installer_created=$false; remote_links_checked=$false; success=$false}
function Check([bool]$condition, [string]$name) {
    $script:report.checks += [ordered]@{name=$name;passed=$condition}
    if (!$condition) { throw "Documentation regression failed: $name" }
}
function Fixture-Directory([string]$name) {
    $path = Join-Path $ArtifactDirectory $name
    New-Item -ItemType Directory -Path $path | Out-Null
    return $path
}
function Expect-Rejection([scriptblock]$action, [string]$message, [string]$name) {
    $rejected = $false
    try { & $action | Out-Null } catch { $rejected = $_.Exception.Message -like $message }
    Check $rejected $name
}
try {
    $app = Fixture-Directory 'installed-app-layout'
    $sourceFiles = @(Get-WindowsDocumentationFiles)
    $sourceHashes = @{}
    foreach ($file in $sourceFiles) { $sourceHashes[$file] = (Get-FileHash -LiteralPath (Join-Path $repository $file)).Hash }
    $documentation = Copy-WindowsDocumentation $app $repository
    Check (($documentation.entry_points -join '|') -eq 'README.md|README_CN.md' -and $documentation.native_guide -eq 'native/README.md') 'bilingual installed entry points preserve the nested native guide'
    foreach ($file in $sourceFiles) {
        Check ((Get-FileHash -LiteralPath (Join-Path $app $file)).Hash -eq $sourceHashes[$file]) "installed document is the exact curated source: $file"
    }
    $prefix = $app + [IO.Path]::DirectorySeparatorChar
    $actual = @(Get-ChildItem -LiteralPath $app -Recurse -File | ForEach-Object { $_.FullName.Substring($prefix.Length).Replace('\', '/') } | Sort-Object)
    Check (($actual -join '|') -ceq ($documentation.files -join '|')) 'installed documentation contains exactly the allowlisted files and generated entry points'
    Check ((Get-FileHash -LiteralPath (Join-Path $app 'THIRD_PARTY.md')).Hash -eq $sourceHashes['native/installer/THIRD_PARTY.md']) 'root notices retain the original complete license text'
    Check ($documentation.local_links_checked -ge 30 -and !$documentation.remote_links_checked) 'both entry points and every bundled Markdown/HTML local link resolve inside the app folder'
    $metadata = Get-Content -LiteralPath (Join-Path $repository 'app-metadata.json') -Raw | ConvertFrom-Json
    $english = [IO.File]::ReadAllText((Join-Path $app 'README.md'))
    $chinese = [IO.File]::ReadAllText((Join-Path $app 'README_CN.md'))
    $expectedEnglish = if ($metadata.release_channel -ceq 'stable') { "$($metadata.version) release" } else { "$($metadata.version) $($metadata.release_channel) preview" }
    Check ($english.Contains($expectedEnglish) -and $chinese.Contains($metadata.version) -and
        (($metadata.release_channel -ceq 'stable') -or $chinese.Contains('尚未作为正式版发布'))) 'installed entry points retain the central product version and release boundary'
    Check (([IO.File]::ReadAllText((Join-Path $app 'README.md')) -match 'can be upgraded in place' -and
        [IO.File]::ReadAllText((Join-Path $app 'README_CN.md')) -match '支持原位升级') -and
        'docs/WINDOWS_SAFE_UPGRADE_CN.md' -in $documentation.files -and
        'docs/RELEASE_UPDATE_REQUIREMENTS_CN.md' -in $documentation.files) 'offline guides describe verified in-place upgrades and include current release requirements and preservation boundaries'
    Check (@($actual | Where-Object { $_ -match '\.(?:py|cpp|bag|mp4|exe)$' }).Count -eq 0) 'documentation copying does not bundle source executables, legacy Python, bags or videos'
    foreach ($file in $sourceFiles) {
        Check ((Get-FileHash -LiteralPath (Join-Path $repository $file)).Hash -eq $sourceHashes[$file]) "documentation copying leaves the repository unchanged: $file"
    }
    $stableRepository = Fixture-Directory 'stable-source'
    foreach ($file in $sourceFiles) {
        $destination = Join-Path $stableRepository $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath (Join-Path $repository $file) -Destination $destination
    }
    $stableMetadata = $metadata | ConvertTo-Json -Depth 5 | ConvertFrom-Json
    $stableMetadata.version = '9.8.7'
    $stableMetadata.release_channel = 'stable'
    $stableMetadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stableRepository 'app-metadata.json') -Encoding utf8
    $stableApp = Fixture-Directory 'stable-app'
    Copy-WindowsDocumentation $stableApp $stableRepository | Out-Null
    $stableEnglish = [IO.File]::ReadAllText((Join-Path $stableApp 'README.md'))
    $stableChinese = [IO.File]::ReadAllText((Join-Path $stableApp 'README_CN.md'))
    Check ($stableEnglish.Contains('9.8.7 release') -and $stableChinese.Contains('9.8.7 正式版') -and
        !$stableEnglish.Contains('development preview') -and !$stableChinese.Contains('尚未作为正式版发布')) 'stable fixture uses its own metadata without retaining a hardcoded preview version'
    $broken = Fixture-Directory 'missing-link'
    [IO.File]::WriteAllText((Join-Path $broken 'README.md'), '[Required guide](docs/absent.md)')
    Expect-Rejection { Assert-WindowsDocumentationLinks $broken } '*link is missing*' 'missing local documentation cannot pass the installed-layout check'
    $escape = Fixture-Directory 'escaping-link'
    [IO.File]::WriteAllText((Join-Path $escape 'README.md'), '[Outside](../outside.md)')
    Expect-Rejection { Assert-WindowsDocumentationLinks $escape } '*link escapes*' 'parent-directory links outside the installed app are rejected'
    $absolute = Fixture-Directory 'absolute-link'
    [IO.File]::WriteAllText((Join-Path $absolute 'README.md'), '[Local machine](C:/unavailable-private/document.md)')
    Expect-Rejection { Assert-WindowsDocumentationLinks $absolute } '*absolute local link*' 'machine-specific absolute local links are rejected'
    $collision = Fixture-Directory 'existing-output'
    $sentinel = Join-Path $collision 'README.md'
    [IO.File]::WriteAllText($sentinel, 'Existing documentation must survive')
    Expect-Rejection { Copy-WindowsDocumentation $collision $repository } '*refuses an existing output*' 'existing documentation is rejected before any copy'
    Check (([IO.File]::ReadAllText($sentinel) -ceq 'Existing documentation must survive') -and
        @(Get-ChildItem -LiteralPath $collision -Recurse -File).Count -eq 1) 'collision rejection preserves all existing content'
    $missingRepository = Fixture-Directory 'missing-source'
    $emptyApp = Fixture-Directory 'empty-output'
    Expect-Rejection { Copy-WindowsDocumentation $emptyApp $missingRepository } '*Missing Windows documentation source*' 'missing curated input is rejected before any copy'
    Check (@(Get-ChildItem -LiteralPath $emptyApp -Force).Count -eq 0) 'missing-source rejection leaves the output empty'
    $report.documentation = $documentation
    $report.source_sha256 = $sourceHashes
    $report.success = $true
} catch {
    $report.error = $_.Exception.Message
    throw
} finally {
    $report | ConvertTo-Json -Depth 7 | Set-Content -Encoding utf8 (Join-Path $ArtifactDirectory 'report.json')
}
Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'report.json')
