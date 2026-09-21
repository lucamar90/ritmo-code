@echo off
REM Crea il pacchetto pronto in dist\ (richiede Python 3.8+ e: pip install pyinstaller winrt-runtime
REM   winrt-Windows.Media.Control winrt-Windows.Foundation winrt-Windows.Foundation.Collections
REM   winrt-Windows.Storage.Streams winrt-Windows.Graphics.Imaging)
cd /d "%~dp0"
set "WORK=%TEMP%\ritmo-pc-monitor-build"
python -m PyInstaller --noconfirm --onefile --console   --name RitmoCodePcMonitor          --icon "%~dp0icon.ico" --workpath "%WORK%" --specpath "%WORK%" --distpath dist --collect-all winrt ritmo_pc_monitor.py || exit /b 1
python -m PyInstaller --noconfirm --onefile --noconsole --name RitmoCodePcMonitor-nascosto --icon "%~dp0icon.ico" --workpath "%WORK%" --specpath "%WORK%" --distpath dist --collect-all winrt ritmo_pc_monitor.py || exit /b 1
if not exist LibreHardwareMonitor\LibreHardwareMonitor.exe powershell -NoProfile -ExecutionPolicy Bypass -File scarica_librehardwaremonitor.ps1 || exit /b 1
robocopy LibreHardwareMonitor dist\LibreHardwareMonitor /e /xf *.pdb *.xml >nul
copy /y installa.bat dist\ >nul
copy /y disinstalla.bat dist\ >nul
copy /y LEGGIMI.txt dist\ >nul
echo Pacchetto pronto in dist\
