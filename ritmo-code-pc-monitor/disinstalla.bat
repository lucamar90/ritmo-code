@echo off
setlocal
REM Ritmo Code PC Monitor - rimuove avvio automatico, attivita', regola firewall (chiede i permessi)
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
taskkill /f /im RitmoCodePcMonitor-nascosto.exe >nul 2>&1
taskkill /f /im RitmoCodePcMonitor.exe >nul 2>&1
del "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Ritmo Code PC Monitor.lnk" >nul 2>&1
schtasks /delete /f /tn "Ritmo Code - LibreHardwareMonitor" >nul 2>&1
taskkill /f /im LibreHardwareMonitor.exe >nul 2>&1
netsh advfirewall firewall delete rule name="Ritmo Code PC Monitor" >nul 2>&1
REM riattiva il LibreHardwareMonitor di un'altra app, se l'installazione l'aveva disattivato
schtasks /query /tn "LibreHardwareMonitor" >nul 2>&1 && schtasks /change /tn "LibreHardwareMonitor" /enable >nul && schtasks /run /tn "LibreHardwareMonitor" >nul
echo Ritmo Code PC Monitor rimosso. La cartella si puo' cancellare.
pause
