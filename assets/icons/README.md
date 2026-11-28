# UAV Studio icon assets

These project-authored icons use the repository's MIT license. The vector source is
`uav-studio.svg`; it combines a graphite-blue rounded tile, a quadrotor and a flight
path with point samples. No third-party stock image is used.

The committed PNG sizes are 16, 32, 48, 64, 128, 256, 512 and 1024 pixels. The ICO
contains 16 through 256 pixel PNG entries for Windows; the ICNS contains the
corresponding macOS PNG blocks through 1024 pixels. `native/tools/render_assets.cpp`
generates these derivatives with Qt SVG rendering.

The same ICO is embedded into the Windows executable, installer and uninstaller.
Start-menu shortcuts, optional desktop shortcuts and project file associations use
the installed executable's icon. Linux installs the SVG and PNG assets under the
standard hicolor icon theme; its desktop entry uses `Icon=uav-studio`.

Icon resource checks do not certify desktop/taskbar appearance or a macOS package.
Those require running the installed application on the target desktop.
