function Get-WindowsDocumentationFiles {
    # Keep this allowlist small: no user media, build artifacts or legacy Python
    # examples belong in the standalone Windows payload.
    return @(
        'LICENSE',
        'native/README.md',
        'native/installer/THIRD_PARTY.md',
        'assets/icons/uav-studio-128.png',
        '.github/workflows/native-windows.yml',
        'docs/REBUILD_ACCEPTANCE_CN.md',
        'docs/ELEMENT_LIFECYCLE_ACCEPTANCE_CN.md',
        'docs/SHARED_CANVAS_MODEL_CN.md',
        'docs/V1_CORE_RELEASE_SCOPE_CN.md',
        'docs/V1_RVIZ_ECOSYSTEM_SCOPE_CN.md',
        'docs/RELEASE_UPDATE_REQUIREMENTS_CN.md',
        'docs/WINDOWS_SAFE_UPGRADE_CN.md',
        'docs/PRECISE_VIDEO_SYNC_CN.md',
        'docs/NATIVE_RVIZ_BACKEND_CN.md',
        'docs/WINDOWS_BACKEND_COMPATIBILITY_CN.md',
        'docs/RVIZ_COMPATIBILITY_MATRIX_CN.md'
    )
}

function Assert-WindowsDocumentationLinks([string]$AppDirectory) {
    $AppDirectory = (Resolve-Path -LiteralPath $AppDirectory).Path
    $prefix = $AppDirectory.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $checked = 0
    foreach ($document in @(Get-ChildItem -LiteralPath $AppDirectory -Recurse -File -Filter '*.md')) {
        $text = [IO.File]::ReadAllText($document.FullName)
        # The shipped guides use inline Markdown and HTML links/images. External
        # addresses remain external; this check does not claim network validation.
        $targets = @([regex]::Matches($text, '\]\(\s*<?([^\s)>]+)>?(?:\s+"[^"]*")?\s*\)') | ForEach-Object { $_.Groups[1].Value })
        $targets += @([regex]::Matches($text, '(?:href|src)\s*=\s*["'']([^"'']+)["'']', [Text.RegularExpressions.RegexOptions]::IgnoreCase) | ForEach-Object { $_.Groups[1].Value })
        foreach ($target in $targets) {
            if ([IO.Path]::IsPathRooted($target) -or $target -match '^file:') { throw "Installed documentation uses an absolute local link: $target" }
            if ($target.StartsWith('#') -or $target -match '^[A-Za-z][A-Za-z0-9+.-]*:') { continue }
            $relative = [Uri]::UnescapeDataString(($target -split '[?#]', 2)[0])
            if (!$relative) { continue }
            if ([IO.Path]::IsPathRooted($relative)) { throw "Installed documentation uses an absolute local link: $target" }
            $resolved = [IO.Path]::GetFullPath((Join-Path $document.DirectoryName $relative))
            if (!$resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Installed documentation link escapes the app directory: $target"
            }
            if (!(Test-Path -LiteralPath $resolved -PathType Leaf)) {
                $source = $document.FullName.Substring($prefix.Length)
                throw "Installed documentation link is missing: $source -> $target"
            }
            ++$checked
        }
    }
    return $checked
}

function Copy-WindowsDocumentation([string]$AppDirectory, [string]$RepositoryRoot) {
    $AppDirectory = (Resolve-Path -LiteralPath $AppDirectory).Path
    $RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
    $files = @(Get-WindowsDocumentationFiles)
    $outputs = @($files) + @('README.md', 'README_CN.md', 'THIRD_PARTY.md')
    # Validate the whole input and all collisions before writing. package.ps1 also
    # enforces fresh CMake staging before any dependency/document deployment.
    foreach ($relative in $files) {
        if (!(Test-Path -LiteralPath (Join-Path $RepositoryRoot $relative) -PathType Leaf)) {
            throw "Missing Windows documentation source: $relative"
        }
    }
    foreach ($relative in $outputs) {
        if (Test-Path -LiteralPath (Join-Path $AppDirectory $relative)) {
            throw "Windows documentation refuses an existing output: $relative"
        }
    }
    $metadata = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'app-metadata.json') -Raw | ConvertFrom-Json
    if ($metadata.version -notmatch '^\d+\.\d+\.\d+$' -or $metadata.release_channel -notmatch '^[A-Za-z0-9.-]+$') {
        throw 'Invalid documentation product version or release channel.'
    }
    $stable = $metadata.release_channel -ceq 'stable'
    $headingEnglish = if ($stable) { 'Windows release' } else { 'Windows development preview' }
    $headingChinese = if ($stable) { 'Windows 正式版' } else { 'Windows 开发预览' }
    $statementEnglish = if ($stable) {
        "This is the C++ / Qt $($metadata.version) release."
    } else {
        "This is the C++ / Qt $($metadata.version) $($metadata.release_channel) preview. This build has not been published as a stable release."
    }
    $statementChinese = if ($stable) {
        "当前为 C++ / Qt $($metadata.version) 正式版。"
    } else {
        "当前为 C++ / Qt $($metadata.version) $($metadata.release_channel) 开发预览，此构建尚未作为正式版发布。"
    }
    foreach ($relative in $files) {
        $destination = Join-Path $AppDirectory $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot $relative) -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'native/installer/THIRD_PARTY.md') -Destination (Join-Path $AppDirectory 'THIRD_PARTY.md')
    # These are installed entry points rather than repository READMEs: retaining
    # the latter would pull historical source/example links into a runtime bundle.
    $english = @'
# UAV Studio — @@HEADING@@

English · [简体中文](README_CN.md)

![UAV Studio icon](assets/icons/uav-studio-128.png)

@@RELEASE_STATEMENT@@
Source implementation and isolated tests do not certify the complete GUI,
native RViz or multi-view export.

Start `uav-studio.exe` in this `app` folder or use the installed shortcut.
The command-line program is `uav-studio-cli.exe` in the same folder.
Keep existing projects and older installations until migration has been checked.
Verified same-product, same-path installations can be upgraded in place after
closing the application. The complete old payload is retained in a separate
backup; arbitrary occupied folders are refused.

- [Usage, projects, exports and command-line guide](native/README.md) (Chinese).
- [Windows and optional backend compatibility](docs/WINDOWS_BACKEND_COMPATIBILITY_CN.md).
- [Safe Windows upgrades and preservation limits](docs/WINDOWS_SAFE_UPGRADE_CN.md).
- [Native RViz dependencies and verification limits](docs/NATIVE_RVIZ_BACKEND_CN.md).
- [Per-plugin RViz compatibility](docs/RVIZ_COMPATIBILITY_MATRIX_CN.md).
- [Core release scope](docs/V1_CORE_RELEASE_SCOPE_CN.md) and [acceptance matrix](docs/REBUILD_ACCEPTANCE_CN.md).
- [MIT license](LICENSE), [third-party notices](THIRD_PARTY.md), and the bundled `licenses` folder.

Standalone Windows features do not require ROS, WSL or Python. An explicitly
included optional Linux RViz host still needs a compatible external ROS/WSL
environment; its presence does not provide native Windows RViz plugins.
The Qt offscreen software backend is suitable for 2D checks, but cannot replace
the real Windows OpenGL backend for CloudView / 3D export acceptance.

This offline documentation is a curated snapshot of the packaged source.
Remote links require internet access and are not checked by the local link audit.
'@
    $chinese = @'
# UAV Studio — @@HEADING@@

[English](README.md) · 简体中文

![UAV Studio 专属图标](assets/icons/uav-studio-128.png)

@@RELEASE_STATEMENT@@
源码实现和专项测试不代表完整 GUI、原生 RViz 或多视角导出已验收。

通过已安装快捷方式，或运行本 `app` 文件夹中的 `uav-studio.exe`。
同目录的 `uav-studio-cli.exe` 提供命令行功能。
完成迁移检查前保留原项目与旧安装。先关闭应用；已验证身份且路径一致的本产品安装支持原位升级，
整个旧程序目录保留在独立备份中，任意占用文件夹不会被自动覆盖。

- [使用、项目、导出及命令行指南](native/README.md)。
- [Windows 与可选后端兼容说明](docs/WINDOWS_BACKEND_COMPATIBILITY_CN.md)。
- [Windows 安全升级与文件保全边界](docs/WINDOWS_SAFE_UPGRADE_CN.md)。
- [原生 RViz 依赖与验收边界](docs/NATIVE_RVIZ_BACKEND_CN.md)。
- [逐插件 RViz 兼容矩阵](docs/RVIZ_COMPATIBILITY_MATRIX_CN.md)。
- [核心发行范围](docs/V1_CORE_RELEASE_SCOPE_CN.md)与[完整验收矩阵](docs/REBUILD_ACCEPTANCE_CN.md)。
- [MIT 许可证](LICENSE)、[第三方说明](THIRD_PARTY.md)与随包 `licenses` 文件夹。

独立 Windows 基础功能不要求 ROS、WSL 或 Python。显式附带的可选 Linux RViz 宿主仍需
兼容的外部 ROS/WSL 环境，不等于提供原生 Windows RViz 插件。
Qt offscreen 软件后端适用于二维检查，不能替代真实 Windows OpenGL 下的 CloudView/三维导出验收。

这些离线文档是打包源码的精选快照。远程链接需要联网，本地链接检查不验证远程地址。
'@
    $english = $english.Replace('@@HEADING@@', $headingEnglish).Replace('@@RELEASE_STATEMENT@@', $statementEnglish)
    $chinese = $chinese.Replace('@@HEADING@@', $headingChinese).Replace('@@RELEASE_STATEMENT@@', $statementChinese)
    [IO.File]::WriteAllText((Join-Path $AppDirectory 'README.md'), $english + "`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $AppDirectory 'README_CN.md'), $chinese + "`n", [Text.UTF8Encoding]::new($false))
    $checkedLinks = Assert-WindowsDocumentationLinks $AppDirectory
    return [ordered]@{
        profile = 'CuratedOfflineDocumentation'
        entry_points = @('README.md', 'README_CN.md')
        native_guide = 'native/README.md'
        files = @($outputs | Sort-Object)
        local_links_checked = $checkedLinks
        remote_links_checked = $false
    }
}
