param([string]$ArtifactDirectory)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
if (!$ArtifactDirectory) { $ArtifactDirectory = Join-Path $repository ("captures/release-gates-" + [Guid]::NewGuid().ToString('N')) }
$ArtifactDirectory = [IO.Path]::GetFullPath($ArtifactDirectory)
if (!$ArtifactDirectory.StartsWith($repository + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Release gate artifacts must remain inside this repository.'
}
if (Test-Path -LiteralPath $ArtifactDirectory) { throw 'Choose a fresh artifact directory.' }
New-Item -ItemType Directory -Path $ArtifactDirectory | Out-Null
$report = [ordered]@{kind='artifact-only-release-gates';checks=@();application_executed=$false;remote_releases_changed=$false;github_runner_executed=$false;success=$false}
function Check([bool]$condition, [string]$name) {
    $script:report.checks += [ordered]@{name=$name;passed=$condition}
    if (!$condition) { throw "Release gate failed: $name" }
}
function Get-ReleaseWriteViolations([string]$text) {
    # Inspect command/action and permission shapes without executing any command.
    # Comments do not change the gate; read-only release queries remain allowed.
    $code = ($text -split "`n" | Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    $patterns = [ordered]@{
        release_cli_write = '(?i)\bgh(?:\.exe)?\s+release\s+(?:create|upload|edit|delete)\b'
        release_action_write = '(?im)^\s*-?\s*uses:\s*(?:softprops/action-gh-release|actions/(?:create-release|upload-release-asset)|ncipollo/release-action)@'
        contents_write = '(?im)^\s*contents:\s*["'']?write["'']?\s*(?:#.*)?$'
        permissions_write_all = '(?im)^\s*permissions:\s*["'']?write-all["'']?\s*(?:#.*)?$'
    }
    foreach ($pattern in $patterns.GetEnumerator()) {
        if ($code -match $pattern.Value) { $pattern.Key }
    }
}
try {
    $blockedFixtures = [ordered]@{
        create = 'gh release create example --latest'
        upload = 'gh release upload example installer.exe'
        edit = 'gh.exe release edit example --latest'
        remove_rollback = 'gh release delete old-tag --yes'
        write_permission = "permissions:`n  contents: write"
        all_write_permission = 'permissions: write-all'
        release_action = '- uses: softprops/action-gh-release@v2'
        legacy_release_action = '- uses: actions/create-release@v1'
        upload_action = '- uses: actions/upload-release-asset@v1'
    }
    foreach ($fixture in $blockedFixtures.GetEnumerator()) {
        Check (@(Get-ReleaseWriteViolations $fixture.Value).Count -gt 0) "rejects $($fixture.Key) without executing it"
    }
    Check (@(Get-ReleaseWriteViolations "permissions:`n  contents: read`n- uses: actions/upload-artifact@v4`ngh release view previous --json assets").Count -eq 0) 'allows artifact upload and read-only release inspection'
    $workflows = @(Get-ChildItem -LiteralPath (Join-Path $repository '.github/workflows') -File | Where-Object Extension -in '.yml', '.yaml')
    foreach ($workflow in $workflows) {
        $text = [IO.File]::ReadAllText($workflow.FullName)
        Check (@(Get-ReleaseWriteViolations $text).Count -eq 0) "workflow remains artifact-only/read-only: $($workflow.Name)"
    }
    foreach ($name in @('native-windows.yml','studio-windows.yml')) {
        $text = [IO.File]::ReadAllText((Join-Path $repository ('.github/workflows/' + $name)))
        Check ($text.Contains('actions/upload-artifact@')) "preserves the installer artifact in $name"
    }
    $native = [IO.File]::ReadAllText((Join-Path $repository '.github/workflows/native-windows.yml'))
    Check ($native -match 'test-release-gates\.ps1\r?\n\s*if \(!\$\?\)') 'native CI executes the release policy regression and fails closed'
    Check (@([regex]::Matches($native, "- '\.github/workflows/\*\*'")).Count -eq 2) 'both push and pull request checks cover every workflow edit'
    $report.success = $true
} catch {
    $report.error = $_.Exception.Message
    throw
} finally {
    $report | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $ArtifactDirectory 'report.json')
}
Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'report.json')
