@echo off
setlocal
pushd "%~dp0" || exit /b 1
set "VENV=.venv\Scripts\python.exe"
set "CHECK=import sys, struct, tkinter; sys.exit(not (sys.version_info >= (3,10) and struct.calcsize('P') == 8))"

if exist "%VENV%" goto check_environment

echo Creating the SLAM FPV Python environment...
for %%V in (3.13 3.12 3.11 3.10 3) do (
    py -%%V -c "%CHECK%" >nul 2>&1
    if not errorlevel 1 (
        py -%%V -m venv .venv
        if errorlevel 1 goto failed
        goto check_environment
    )
)
python -c "%CHECK%" >nul 2>&1
if errorlevel 1 (
    echo Install 64-bit Python 3.10 or newer with Tcl/Tk support, then try again.
    goto failed
)
python -m venv .venv
if errorlevel 1 goto failed

:check_environment
"%VENV%" -c "%CHECK%"
if errorlevel 1 (
    echo The existing .venv cannot use this launcher.
    echo Rename .venv to .venv-old, then double-click this file again.
    goto failed
)

"%VENV%" -c "import hashlib,pathlib,sys; marker=pathlib.Path('.venv/slam-fpv-install.sha256'); sys.exit(not (marker.is_file() and marker.read_text().strip()==hashlib.sha256(pathlib.Path('pyproject.toml').read_bytes()).hexdigest()))"
if errorlevel 1 goto install
"%VENV%" -c "import slam_fpv.gui, vtk, imageio_ffmpeg" >nul 2>&1
if errorlevel 1 goto install
goto launch

:install
echo Installing SLAM FPV and its dependencies. First launch needs Internet access.
"%VENV%" -m pip install -e .
if errorlevel 1 goto failed
"%VENV%" -c "import hashlib,pathlib; pathlib.Path('.venv/slam-fpv-install.sha256').write_text(hashlib.sha256(pathlib.Path('pyproject.toml').read_bytes()).hexdigest())"
if errorlevel 1 goto failed

:launch
"%VENV%" -m slam_fpv.gui
if errorlevel 1 goto failed
popd
exit /b 0

:failed
echo.
echo SLAM FPV could not start. See the message above.
pause
popd
exit /b 1
