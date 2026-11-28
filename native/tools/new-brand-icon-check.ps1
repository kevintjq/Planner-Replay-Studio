param(
    [Parameter(Mandatory = $true)][string]$ApplicationExecutable,
    [Parameter(Mandatory = $true)][string]$InstallerExecutable,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory
)
$ErrorActionPreference = 'Stop'
$ApplicationExecutable = (Resolve-Path -LiteralPath $ApplicationExecutable).Path
$InstallerExecutable = (Resolve-Path -LiteralPath $InstallerExecutable).Path
$ArtifactDirectory = [System.IO.Path]::GetFullPath($ArtifactDirectory)
New-Item -ItemType Directory -Force -Path $ArtifactDirectory | Out-Null
$drawingReferences = @([System.Drawing.Icon].Assembly.Location, [System.Drawing.Color].Assembly.Location)
$drawingReferences += Get-ChildItem (Split-Path -Parent ([System.Drawing.Icon].Assembly.Location)) -Filter 'System.Private.Windows.*.dll' |
    Select-Object -ExpandProperty FullName
Add-Type -ReferencedAssemblies $drawingReferences -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class UavStudioBrandIconCheck {
    [DllImport("shell32.dll", CharSet=CharSet.Unicode)]
    static extern uint ExtractIconEx(string file, int index, IntPtr[] large, IntPtr[] small, uint count);
    [DllImport("user32.dll")] static extern bool DestroyIcon(IntPtr icon);
    public static uint Save(string file, string prefix) {
        var large = new IntPtr[1]; var small = new IntPtr[1];
        uint groups = ExtractIconEx(file, -1, null, null, 0);
        if (ExtractIconEx(file, 0, large, small, 1) == 0)
            throw new Exception("No Windows Shell icon in " + file);
        try {
            using (var icon = Icon.FromHandle(large[0]))
            using (var bitmap = icon.ToBitmap()) bitmap.Save(prefix + "-32.png", ImageFormat.Png);
            using (var icon = Icon.FromHandle(small[0]))
            using (var bitmap = icon.ToBitmap()) bitmap.Save(prefix + "-16.png", ImageFormat.Png);
        } finally {
            if (large[0] != IntPtr.Zero) DestroyIcon(large[0]);
            if (small[0] != IntPtr.Zero) DestroyIcon(small[0]);
        }
        return groups;
    }
}
'@
$appGroups = [UavStudioBrandIconCheck]::Save($ApplicationExecutable, (Join-Path $ArtifactDirectory 'app-icon'))
$installerGroups = [UavStudioBrandIconCheck]::Save($InstallerExecutable, (Join-Path $ArtifactDirectory 'installer-icon'))
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
$sizes = @()
foreach ($size in @(16, 32, 48, 64, 128, 256, 512, 1024)) {
    $image = [System.Drawing.Image]::FromFile((Join-Path $repository "assets/icons/uav-studio-$size.png"))
    try {
        if ($image.Width -ne $size -or $image.Height -ne $size) { throw "Incorrect icon PNG dimensions: $size" }
        $sizes += $size
    } finally { $image.Dispose() }
}
[ordered]@{
    png_sizes_verified = $sizes; application_icon_groups = $appGroups; installer_icon_groups = $installerGroups
    shell_extraction_passed = ($appGroups -gt 0 -and $installerGroups -gt 0)
    desktop_or_taskbar_visual_acceptance = $false
} | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $ArtifactDirectory 'shell-icons.json')
Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'shell-icons.json')
