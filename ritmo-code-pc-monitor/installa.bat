@echo off
setlocal
REM ============================================================================
REM  Ritmo Code PC Monitor - installazione (chiede i permessi di amministratore)
REM   1. regola firewall: porta 8765 aperta solo alla rete locale
REM   2. LibreHardwareMonitor della cartella come attivita' all'accesso (sensori completi)
REM   3. avvio automatico del monitor (versione nascosta) e avvio immediato
REM   4. apre la pagina per collegare il PC al dispositivo
REM ============================================================================
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
cd /d "%~dp0"

set "EXE=%~dp0RitmoCodePcMonitor-nascosto.exe"
if not exist "%EXE%" (
    echo Manca RitmoCodePcMonitor-nascosto.exe in questa cartella: usa la cartella dist\ compilata.
    pause
    exit /b 1
)

echo [1/4] Firewall: porta 8765 per la rete locale
netsh advfirewall firewall delete rule name="Ritmo Code PC Monitor" >nul 2>&1
netsh advfirewall firewall add rule name="Ritmo Code PC Monitor" dir=in action=allow protocol=TCP localport=8765 remoteip=LocalSubnet profile=any >nul

echo [2/4] LibreHardwareMonitor (CPU, GPU, scheda madre, RAM, dischi)
REM un solo LibreHardwareMonitor alla volta: si ferma quello gia' attivo (anche di altre app)
schtasks /query /tn "LibreHardwareMonitor" >nul 2>&1 && schtasks /change /tn "LibreHardwareMonitor" /disable >nul
taskkill /f /im LibreHardwareMonitor.exe >nul 2>&1
schtasks /create /f /tn "Ritmo Code - LibreHardwareMonitor" /sc onlogon /rl highest /tr "\"%~dp0LibreHardwareMonitor\LibreHardwareMonitor.exe\"" >nul
schtasks /run /tn "Ritmo Code - LibreHardwareMonitor" >nul

echo [3/4] Avvio automatico del monitor
taskkill /f /im RitmoCodePcMonitor-nascosto.exe >nul 2>&1
set "LINK=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Ritmo Code PC Monitor.lnk"
powershell -NoProfile -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('%LINK%');$s.TargetPath='%EXE%';$s.WorkingDirectory='%~dp0';$s.Save()"
start "" "%EXE%"

echo [4/4] Apro la pagina per collegare il PC al dispositivo...
timeout /t 4 /nobreak >nul
start "" http://127.0.0.1:8765/

echo.
echo Fatto. Nella pagina aperta scegli il dispositivo, inserisci il PIN e premi "Collega questo PC".
pause
