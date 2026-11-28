param([switch]$Write)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
$metadata = Get-Content -LiteralPath (Join-Path $repository 'app-metadata.json') -Raw | ConvertFrom-Json
if ($metadata.schema_version -ne 1 -or $metadata.version -notmatch '^\d+\.\d+\.\d+$' -or
    $metadata.github_url -cne "https://github.com/$($metadata.repository_full_name)" -or
    !$metadata.author -or !$metadata.email) { throw 'Invalid central product metadata.' }
$displayVersion = $metadata.version
if ($metadata.release_channel -and $metadata.release_channel -ne 'stable') { $displayVersion += " · $($metadata.release_channel)" }
$begin = '<!-- BEGIN PRODUCT METADATA -->'
$end = '<!-- END PRODUCT METADATA -->'
$english = @(
    $begin,
    "Maintained by **$($metadata.author)** · [$($metadata.email)](mailto:$($metadata.email))",
    '',
    "Source version: **$displayVersion** · [GitHub]($($metadata.github_url)) · [Report an issue]($($metadata.github_url)/issues/new/choose) · [Releases]($($metadata.github_url)/releases) · [Roadmap](#roadmap)",
    $end
)
$chinese = @(
    $begin,
    "开发者：**$($metadata.author)** · [$($metadata.email)](mailto:$($metadata.email))",
    '',
    "源码版本：**$displayVersion** · [GitHub]($($metadata.github_url)) · [问题反馈]($($metadata.github_url)/issues/new/choose) · [发行与更新]($($metadata.github_url)/releases) · [更新路线图](#roadmap--后续计划)",
    $end
)
foreach ($entry in @(@{file='README.md';lines=$english}, @{file='README_CN.md';lines=$chinese})) {
    $path = Join-Path $repository $entry.file
    $text = [IO.File]::ReadAllText($path)
    $newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $expected = $entry.lines -join $newline
    $pattern = '(?s)' + [regex]::Escape($begin) + '.*?' + [regex]::Escape($end)
    $matches = [regex]::Matches($text, $pattern)
    if ($matches.Count -ne 1) { throw "Exactly one product metadata block is required in $($entry.file)." }
    if ($matches[0].Value -cne $expected) {
        if (!$Write) { throw "Product metadata is stale in $($entry.file). Run native/tools/sync-project-metadata.ps1 -Write." }
        $text = $text.Substring(0, $matches[0].Index) + $expected + $text.Substring($matches[0].Index + $matches[0].Length)
        [IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
    }
}
Write-Output "Product metadata is synchronized: $($metadata.app_name) $displayVersion / $($metadata.author)."
